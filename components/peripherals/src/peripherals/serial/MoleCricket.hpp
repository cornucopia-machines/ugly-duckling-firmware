#pragma once

/*
 * Mole Cricket: an I²C-to-UART extension board (mole-cricket-hardware) with an SC16IS752 and two
 * sensor terminals, A and B, on the bridge's two UARTs.
 *
 * Board wiring the firmware depends on:
 *   - 11.0592 MHz crystal: 9600 baud is exact (divisor 72).
 *   - GPIO0 drives the probe supply load switch (TPS22917) for both terminals; a pull-down keeps the
 *     probes off until the bridge is configured.
 *   - GPIO1 drives the status LED, active low.
 *   - IRQ is not wired: the host polls RXLVL.
 *
 * Both probes hang off the one supply switch, and the host's port supply is budgeted for one probe
 * measuring at a time (~40 mA each). So the board hands out one session at a time: opening a session
 * on either terminal blocks until the other terminal's session ends.
 *
 * Pure C++ apart from the injected bus and delay, so it compiles in native unit tests.
 */

#include "Sc16is752.hpp"
#include "SerialPort.hpp"
#include <Log.hpp>
#include <Result.hpp>
#include <drivers/SwitchedRail.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>

namespace cornucopia::ugly_duckling::peripherals::serial {

LOGGING_TAG(MOLE_CRICKET, "mole-cricket")

class MoleCricket : public std::enable_shared_from_this<MoleCricket> {
public:
    using Channel = Sc16is752::Channel;

    static constexpr uint32_t CRYSTAL_HZ = 11'059'200;
    static constexpr uint8_t GPIO_PROBE_POWER = 1 << 0;
    static constexpr uint8_t GPIO_LED = 1 << 1;    // active low
    static constexpr uint32_t BAUD = 9600;
    static constexpr std::chrono::milliseconds POLL_INTERVAL { 5 };

    static Result<std::shared_ptr<MoleCricket>> create(std::unique_ptr<ISc16is752Bus> bus, std::shared_ptr<kernel::drivers::SwitchedRail> rail, DelayFunction delay, std::string description) {
        auto board = std::shared_ptr<MoleCricket>(new MoleCricket(std::move(bus), std::move(rail), std::move(delay), std::move(description)));
        TRY(board->railDevice.initialize());
        return board;
    }

    // A link to the sensor on terminal A or B
    std::shared_ptr<ISerialLink> link(Channel channel) {
        return std::make_shared<Link>(shared_from_this(), channel);
    }

    const std::string& getDescription() const {
        return description;
    }

private:
    class Session final : public ISerialSession {
    public:
        Session(std::shared_ptr<MoleCricket> board, Channel channel, std::unique_lock<std::mutex> lock, kernel::drivers::SwitchedRail::Session railSession)
            : board(std::move(board))
            , lock(std::move(lock))
            , railSession(std::move(railSession))
            , port(this->board->chip, channel, POLL_INTERVAL)
            , channel(channel) {
        }

        ~Session() override {
            board->powerDown(channel, port);
        }

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        Result<> write(std::string_view data) override {
            return port.write(data);
        }

        Result<std::string> readUntil(char terminator, size_t maxLength, std::chrono::milliseconds timeout) override {
            return port.readUntil(terminator, maxLength, timeout);
        }

        Result<> flushRx() override {
            return port.flushRx();
        }

    private:
        const std::shared_ptr<MoleCricket> board;
        std::unique_lock<std::mutex> lock;
        // Keeps the board powered until the destructor has powered the probe down
        kernel::drivers::SwitchedRail::Session railSession;
        Sc16is752Port port;
        const Channel channel;
    };

    class Link final : public ISerialLink {
    public:
        Link(std::shared_ptr<MoleCricket> board, Channel channel)
            : board(std::move(board))
            , channel(channel) {
        }

        Result<std::unique_ptr<ISerialSession>> open() override {
            return board->open(channel);
        }

        std::string describe() const override {
            return board->description + ", terminal " + (channel == Channel::A ? "A" : "B");
        }

    private:
        const std::shared_ptr<MoleCricket> board;
        const Channel channel;
    };

    MoleCricket(std::unique_ptr<ISc16is752Bus> bus, std::shared_ptr<kernel::drivers::SwitchedRail> rail, DelayFunction delay, std::string description)
        : chip(std::move(bus), CRYSTAL_HZ, std::move(delay))
        , description(std::move(description))
        // The bridge loses its UART and GPIO setup whenever the rail turns off
        , railDevice(std::move(rail), std::chrono::microseconds::zero(), [this]() { return initialize(); }) {
    }

    Result<> initialize() {
        if (auto present = chip.checkPresent(); !present) {
            return failure("Mole Cricket not found (" + description + "): " + present.error().message
                + ". Check the cable and JP1: both jumpers fitted, A1/A0 at VDD/VDD = 0x48, "
                  "VDD/GND = 0x49, GND/VDD = 0x4C, GND/GND = 0x4D");
        }
        // After power-on all GPIOs are inputs and the pull-down holds the probe supply off; after a
        // host restart they may still be outputs with the probe on. Either way, set the levels
        // before the direction, so neither pin glitches.
        TRY(chip.setGpio(GPIO_PROBE_POWER | GPIO_LED, GPIO_LED));
        TRY(chip.setGpioDirection(GPIO_PROBE_POWER | GPIO_LED));
        for (auto channel : { Channel::A, Channel::B }) {
            TRY(chip.configureUart(channel, BAUD));
            TRY(chip.setSleep(channel, true));
        }
        LOGTI(MOLE_CRICKET, "Mole Cricket initialized (%s)", description.c_str());
        return {};
    }

    Result<std::unique_ptr<ISerialSession>> open(Channel channel) {
        std::unique_lock lock(mutex);
        auto railSession = TRY(railDevice.power());
        TRY(chip.setSleep(channel, false));
        TRY(chip.resetFifos(channel));
        // Probe supply on, LED on (active low)
        TRY(chip.setGpio(GPIO_PROBE_POWER | GPIO_LED, GPIO_PROBE_POWER));
        return std::make_unique<Session>(shared_from_this(), channel, std::move(lock), std::move(railSession));
    }

    // Called with the session's lock still held
    void powerDown(Channel channel, Sc16is752Port& port) {
        if (auto result = chip.setGpio(GPIO_PROBE_POWER | GPIO_LED, GPIO_LED); !result) {
            LOGTW(MOLE_CRICKET, "Failed to switch probe supply off (%s): %s",
                description.c_str(), result.error().message.c_str());
        }
        // A probe that loses power can leave junk in the RX FIFO, and sleep mode is not entered
        // while the RX FIFO holds data
        if (auto result = port.flushRx(); !result) {
            LOGTW(MOLE_CRICKET, "Failed to flush RX (%s): %s",
                description.c_str(), result.error().message.c_str());
        }
        if (auto result = chip.setSleep(channel, true); !result) {
            LOGTW(MOLE_CRICKET, "Failed to put UART to sleep (%s): %s",
                description.c_str(), result.error().message.c_str());
        }
    }

    Sc16is752 chip;
    const std::string description;
    kernel::drivers::RailDevice railDevice;
    std::mutex mutex;
};

}    // namespace cornucopia::ugly_duckling::peripherals::serial
