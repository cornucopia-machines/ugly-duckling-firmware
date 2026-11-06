#pragma once

/*
 * The board's own RS485 port: one of the ESP's UARTs behind an auto-direction transceiver
 * (THVD1406 on MK14), shared by every sensor on the bus.
 *
 * Board wiring the firmware depends on:
 *   - The transceiver switches direction by itself from the TX line, so there is no DE/RE pin to
 *     drive. While we transmit, its receiver output floats, so whatever the UART receives during our
 *     own transmission is noise; write() discards it.
 *   - The transceiver and the probes are powered from the switched rail. The transceiver's logic
 *     inputs have pull-ups to its own supply, so the UART is attached to the pins only while a
 *     session holds the rail. In between, both pins float: an idle-high TX, or a pull-up on RX, would
 *     back-power the transceiver while the rail is off.
 *
 * Sensors on the bus may run at different baud rates: each link sets its own when it opens a
 * session. One session at a time, as the bus is half-duplex.
 */

#include "SerialPort.hpp"
#include <EspError.hpp>
#include <Log.hpp>
#include <Pin.hpp>
#include <PowerManager.hpp>
#include <Result.hpp>
#include <drivers/SwitchedRail.hpp>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace cornucopia::ugly_duckling::peripherals::serial {

LOGGING_TAG(RS485, "rs485")

class Rs485Bus final
    : public ISerialBus,
      public std::enable_shared_from_this<Rs485Bus> {
public:
    // Must exceed the hardware FIFO (128 bytes on the ESP32-C6); sensor responses are far shorter
    static constexpr int RX_BUFFER_SIZE = 256;

    static Result<std::shared_ptr<Rs485Bus>> create(uart_port_t port, const InternalPinPtr& tx, const InternalPinPtr& rx, std::shared_ptr<kernel::drivers::SwitchedRail> rail) {
        uart_config_t config {};
        // Each link sets its own baud rate when it opens a session
        config.baud_rate = 9600;
        config.data_bits = UART_DATA_8_BITS;
        config.parity = UART_PARITY_DISABLE;
        config.stop_bits = UART_STOP_BITS_1;
        config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        // The XTAL clock keeps the baud rate exact while power management scales the APB clock
        config.source_clk = UART_SCLK_XTAL;
        ESP_TRY(uart_driver_install(port, RX_BUFFER_SIZE, 0, 0, nullptr, 0));
        ESP_TRY(uart_param_config(port, &config));
        auto bus = std::shared_ptr<Rs485Bus>(new Rs485Bus(port, tx, rx, std::move(rail)));
        bus->detachPins();
        LOGTI(RS485, "RS485 bus on UART%d (TX %s, RX %s)",
            static_cast<int>(port), tx->getName().c_str(), rx->getName().c_str());
        return bus;
    }

    std::shared_ptr<ISerialLink> link(uint32_t baud) override {
        return std::make_shared<Link>(shared_from_this(), baud);
    }

private:
    class Session final : public ISerialSession {
    public:
        Session(std::shared_ptr<Rs485Bus> bus, uint32_t baud, std::unique_lock<std::mutex> lock, kernel::drivers::SwitchedRail::Session railSession)
            : bus(std::move(bus))
            , baud(baud)
            , lock(std::move(lock))
            , railSession(std::move(railSession))
            , noSleep(this->bus->noSleepLock) {
        }

        // Detaches the pins before the members release the rail
        ~Session() override {
            uart_wait_tx_done(bus->port, toTicks(txTime(Rs485Bus::RX_BUFFER_SIZE)));
            bus->detachPins();
        }

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        Result<> write(std::string_view data) override {
            int written = uart_write_bytes(bus->port, data.data(), data.size());
            if (std::cmp_not_equal(written, data.size())) {
                return failure("RS485: queued only " + std::to_string(written) + " of " + std::to_string(data.size()) + " bytes");
            }
            ESP_TRY(uart_wait_tx_done(bus->port, toTicks(txTime(data.size()) + std::chrono::milliseconds(50))));
            // The transceiver's receiver output floats while we transmit
            ESP_TRY(uart_flush_input(bus->port));
            return {};
        }

        Result<std::string> read(size_t length, std::chrono::milliseconds timeout) override {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            std::string data(length, '\0');
            size_t received = 0;
            while (received < length) {
                int count = uart_read_bytes(bus->port, data.data() + received, length - received, toTicks(remaining(deadline)));
                if (count < 0) {
                    return failure("RS485: read failed");
                }
                if (count == 0) {
                    return failure(received == 0
                            ? "no response within " + std::to_string(timeout.count()) + " ms"
                            : "only " + std::to_string(received) + " of " + std::to_string(length)
                                + " bytes within " + std::to_string(timeout.count()) + " ms");
                }
                received += static_cast<size_t>(count);
            }
            return data;
        }

        Result<std::string> readUntil(char terminator, size_t maxLength, std::chrono::milliseconds timeout) override {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            std::string line;
            while (line.size() < maxLength) {
                char c = 0;
                int count = uart_read_bytes(bus->port, &c, 1, toTicks(remaining(deadline)));
                if (count < 0) {
                    return failure("RS485: read failed");
                }
                if (count == 0) {
                    return failure(line.empty()
                            ? "no response within " + std::to_string(timeout.count()) + " ms"
                            : "incomplete response within " + std::to_string(timeout.count()) + " ms");
                }
                if (c == terminator) {
                    return line;
                }
                line += c;
            }
            return failure("no terminator within " + std::to_string(maxLength) + " bytes");
        }

        Result<> flushRx() override {
            ESP_TRY(uart_flush_input(bus->port));
            return {};
        }

        void waitPoweredFor(std::chrono::milliseconds duration) override {
            railSession.waitPoweredFor(duration);
        }

    private:
        // How long `bytes` take on the wire at 10 bits each (8N1)
        std::chrono::milliseconds txTime(size_t bytes) const {
            return std::chrono::milliseconds((bytes * 10 * 1000 + baud - 1) / baud);
        }

        static std::chrono::milliseconds remaining(std::chrono::steady_clock::time_point deadline) {
            auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            return left > std::chrono::milliseconds::zero() ? left : std::chrono::milliseconds::zero();
        }

        // Rounds up, so a short wait isn't cut to zero ticks
        static TickType_t toTicks(std::chrono::milliseconds duration) {
            return static_cast<TickType_t>((duration.count() + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS);
        }

        const std::shared_ptr<Rs485Bus> bus;
        const uint32_t baud;
        std::unique_lock<std::mutex> lock;
        kernel::drivers::SwitchedRail::Session railSession;
        // Light sleep would stop the UART mid-frame
        kernel::PowerManagementLockGuard noSleep;
    };

    class Link final : public ISerialLink {
    public:
        Link(std::shared_ptr<Rs485Bus> bus, uint32_t baud)
            : bus(std::move(bus))
            , baud(baud) {
        }

        Result<std::unique_ptr<ISerialSession>> open() override {
            return bus->open(baud);
        }

        std::string describe() const override {
            return "RS485, " + std::to_string(baud) + " baud";
        }

    private:
        const std::shared_ptr<Rs485Bus> bus;
        const uint32_t baud;
    };

    Rs485Bus(uart_port_t port, InternalPinPtr tx, InternalPinPtr rx, std::shared_ptr<kernel::drivers::SwitchedRail> rail)
        : port(port)
        , tx(std::move(tx))
        , rx(std::move(rx))
        , rail(std::move(rail)) {
    }

    Result<std::unique_ptr<ISerialSession>> open(uint32_t baud) {
        if (baud == 0) {
            return failure("RS485: baud rate must be positive");
        }
        std::unique_lock lock(mutex);
        // Power the transceiver before attaching the pins (see the top of the file)
        auto railSession = rail->acquire();
        // From here the session detaches the pins and releases the rail even if setting up fails
        auto session = std::make_unique<Session>(shared_from_this(), baud, std::move(lock), std::move(railSession));
        ESP_TRY(uart_set_baudrate(port, baud));
        ESP_TRY(uart_set_pin(port, tx->getGpio(), rx->getGpio(), UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        // Keeps RX idle while the transceiver's receiver output floats during our own transmission
        ESP_TRY(gpio_pullup_en(rx->getGpio()));
        ESP_TRY(uart_flush_input(port));
        return std::unique_ptr<ISerialSession>(std::move(session));
    }

    // Leaves both pins floating; uart_set_pin() attaches them again
    void detachPins() {
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_output_disable(tx->getGpio()));
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_pull_mode(tx->getGpio(), GPIO_FLOATING));
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_pull_mode(rx->getGpio(), GPIO_FLOATING));
    }

    const uart_port_t port;
    const InternalPinPtr tx;
    const InternalPinPtr rx;
    const std::shared_ptr<kernel::drivers::SwitchedRail> rail;
    std::mutex mutex;
    kernel::PowerManagementLock noSleepLock { "rs485", ESP_PM_NO_LIGHT_SLEEP };
};

}    // namespace cornucopia::ugly_duckling::peripherals::serial
