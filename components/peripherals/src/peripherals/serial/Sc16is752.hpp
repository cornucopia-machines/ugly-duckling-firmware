#pragma once

/*
 * NXP SC16IS752: dual UART with an I²C interface (datasheet: sc16is752_sc16is762.pdf).
 *
 * Pure C++: the register traffic goes through ISc16is752Bus and waiting through a delay function,
 * so the driver compiles in native unit tests. The ESP-IDF adapter that puts the bus on an
 * I2CDevice lives in Sc16is752I2CBus.hpp.
 *
 * The driver assumes IRQ is not wired and polls RXLVL.
 */

#include "SerialPort.hpp"
#include <Result.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cornucopia::ugly_duckling::peripherals::serial {

using cornucopia::ugly_duckling::kernel::failure;
using cornucopia::ugly_duckling::kernel::Result;

// Raw register access. `subaddress` is the full I²C register byte, with the register and channel
// already encoded (see Sc16is752::subaddress()). The burst operations transfer `length` bytes to or
// from the same register without auto-increment, which is how the RHR / THR FIFOs are read and written.
struct ISc16is752Bus {
    virtual ~ISc16is752Bus() = default;

    virtual Result<uint8_t> readRegister(uint8_t subaddress) = 0;
    virtual Result<> writeRegister(uint8_t subaddress, uint8_t value) = 0;
    virtual Result<> readBurst(uint8_t subaddress, uint8_t* buffer, size_t length) = 0;
    virtual Result<> writeBurst(uint8_t subaddress, const uint8_t* buffer, size_t length) = 0;
};

using DelayFunction = std::function<void(std::chrono::milliseconds)>;

class Sc16is752 {
public:
    enum class Channel : uint8_t {
        A = 0,
        B = 1,
    };

    // General register set (LCR[7] = 0)
    static constexpr uint8_t REG_RHR = 0x00;    // read
    static constexpr uint8_t REG_THR = 0x00;    // write
    static constexpr uint8_t REG_IER = 0x01;
    static constexpr uint8_t REG_FCR = 0x02;    // write
    static constexpr uint8_t REG_LCR = 0x03;
    static constexpr uint8_t REG_MCR = 0x04;
    static constexpr uint8_t REG_LSR = 0x05;
    static constexpr uint8_t REG_SPR = 0x07;
    static constexpr uint8_t REG_TXLVL = 0x08;
    static constexpr uint8_t REG_RXLVL = 0x09;
    // These four apply to both channels; they are addressed with channel A
    static constexpr uint8_t REG_IODIR = 0x0A;
    static constexpr uint8_t REG_IOSTATE = 0x0B;
    static constexpr uint8_t REG_IOINTENA = 0x0C;
    static constexpr uint8_t REG_IOCONTROL = 0x0E;
    // Special register set (LCR[7] = 1, LCR != 0xBF)
    static constexpr uint8_t REG_DLL = 0x00;
    static constexpr uint8_t REG_DLH = 0x01;
    // Enhanced register set (LCR = 0xBF)
    static constexpr uint8_t REG_EFR = 0x02;

    static constexpr uint8_t LCR_8N1 = 0x03;
    static constexpr uint8_t LCR_DIVISOR_LATCH = 0x80;
    static constexpr uint8_t LCR_ENHANCED_ACCESS = 0xBF;
    static constexpr uint8_t EFR_ENHANCED_FUNCTIONS = 0x10;
    static constexpr uint8_t IER_SLEEP = 0x10;
    static constexpr uint8_t FCR_FIFO_ENABLE = 0x01;
    static constexpr uint8_t FCR_RX_RESET = 0x02;
    static constexpr uint8_t FCR_TX_RESET = 0x04;

    static constexpr size_t FIFO_SIZE = 64;

    static constexpr uint8_t subaddress(uint8_t reg, Channel channel = Channel::A) {
        // Bit 7 unused, bits 6:3 register, bits 2:1 channel, bit 0 unused (Table 33)
        return static_cast<uint8_t>((reg << 3) | (static_cast<uint8_t>(channel) << 1));
    }

    // Divisor for `baud` from `crystalHz`, with the MCR[7] prescaler at 1. Fails if the nearest
    // divisor is off by more than 1 %.
    static Result<uint16_t> divisorFor(uint32_t crystalHz, uint32_t baud) {
        if (baud == 0) {
            return failure("SC16IS752: baud rate must be positive");
        }
        uint32_t divisor = (crystalHz + (8 * baud)) / (16 * baud);
        if (divisor == 0 || divisor > 0xFFFF) {
            return failure("SC16IS752: baud rate " + std::to_string(baud) + " out of range");
        }
        uint32_t actual = crystalHz / (16 * divisor);
        if (std::abs(static_cast<int64_t>(actual) - static_cast<int64_t>(baud)) * 100 > baud) {
            return failure("SC16IS752: cannot make " + std::to_string(baud) + " baud from a "
                + std::to_string(crystalHz) + " Hz crystal");
        }
        return static_cast<uint16_t>(divisor);
    }

    Sc16is752(std::unique_ptr<ISc16is752Bus> bus, uint32_t crystalHz, DelayFunction delay)
        : bus(std::move(bus))
        , crystalHz(crystalHz)
        , delay(std::move(delay)) {
    }

    // Checks that the chip answers: writes and reads back the scratch-pad register.
    //
    // There is deliberately no software reset (IOControl SRESET): the chip resets while it
    // acknowledges that write, so the write always reports a NACK, which i2cdev retries and logs as
    // an error. Nothing needs it either: configureUart() and the GPIO setup rewrite every register
    // we rely on, including state left over when the host restarts but the bridge stays powered.
    Result<> checkPresent() {
        for (uint8_t pattern : std::array<uint8_t, 2> { 0x55, 0xAA }) {
            TRY(bus->writeRegister(subaddress(REG_SPR), pattern));
            uint8_t readBack = TRY(bus->readRegister(subaddress(REG_SPR)));
            if (readBack != pattern) {
                return failure("SC16IS752: scratch-pad register read back 0x" + toHex(readBack)
                    + " instead of 0x" + toHex(pattern));
            }
        }
        return {};
    }

    // 8N1 at `baud`, FIFOs enabled and cleared, interrupts off, enhanced functions on (needed for
    // sleep mode).
    Result<> configureUart(Channel channel, uint32_t baud) {
        uint16_t divisor = TRY(divisorFor(crystalHz, baud));
        // The chip may still be asleep from before the host restarted, and the divisor must not be
        // written in sleep mode. IER is in the general register set, so make LCR select that first.
        TRY(bus->writeRegister(subaddress(REG_LCR, channel), LCR_8N1));
        TRY(bus->writeRegister(subaddress(REG_IER, channel), 0x00));
        TRY(bus->writeRegister(subaddress(REG_LCR, channel), LCR_ENHANCED_ACCESS));
        TRY(bus->writeRegister(subaddress(REG_EFR, channel), EFR_ENHANCED_FUNCTIONS));
        TRY(bus->writeRegister(subaddress(REG_LCR, channel), LCR_DIVISOR_LATCH | LCR_8N1));
        TRY(bus->writeRegister(subaddress(REG_DLL, channel), static_cast<uint8_t>(divisor & 0xFF)));
        TRY(bus->writeRegister(subaddress(REG_DLH, channel), static_cast<uint8_t>(divisor >> 8)));
        TRY(bus->writeRegister(subaddress(REG_LCR, channel), LCR_8N1));
        TRY(bus->writeRegister(subaddress(REG_MCR, channel), 0x00));    // prescaler ÷1
        TRY(bus->writeRegister(subaddress(REG_IER, channel), 0x00));
        TRY(resetFifos(channel));
        return {};
    }

    Result<> setSleep(Channel channel, bool enabled) {
        return bus->writeRegister(subaddress(REG_IER, channel), enabled ? IER_SLEEP : 0x00);
    }

    Result<> resetFifos(Channel channel) {
        TRY(bus->writeRegister(subaddress(REG_FCR, channel), FCR_FIFO_ENABLE | FCR_RX_RESET | FCR_TX_RESET));
        // The FIFOs need 2 crystal periods after a reset before RHR / THR are touched; the next I²C
        // transaction alone takes far longer than that.
        return {};
    }

    // GPIO: `outputs` is the mask of pins driven by the chip; the rest are inputs.
    Result<> setGpioDirection(uint8_t outputs) {
        return bus->writeRegister(subaddress(REG_IODIR), outputs);
    }

    // Sets the pins in `mask` to the corresponding bits of `values`, leaving the other outputs as
    // they were.
    Result<> setGpio(uint8_t mask, uint8_t values) {
        auto next = static_cast<uint8_t>((ioState & ~mask) | (values & mask));
        TRY(bus->writeRegister(subaddress(REG_IOSTATE), next));
        ioState = next;
        return {};
    }

    Result<size_t> rxLevel(Channel channel) {
        uint8_t level = TRY(bus->readRegister(subaddress(REG_RXLVL, channel)));
        return std::min<size_t>(level, FIFO_SIZE);
    }

    Result<> readFifo(Channel channel, uint8_t* buffer, size_t length) {
        if (length == 0) {
            return {};
        }
        return bus->readBurst(subaddress(REG_RHR, channel), buffer, length);
    }

    // Writes `data`, waiting for space in the TX FIFO as needed. Gives up if the FIFO doesn't drain
    // within `timeout`.
    Result<> write(Channel channel, std::string_view data, std::chrono::milliseconds timeout, std::chrono::milliseconds pollInterval) {
        auto waited = std::chrono::milliseconds::zero();
        while (!data.empty()) {
            size_t space = std::min<size_t>(TRY(bus->readRegister(subaddress(REG_TXLVL, channel))), FIFO_SIZE);
            if (space == 0) {
                if (waited >= timeout) {
                    return failure("SC16IS752: TX FIFO did not drain");
                }
                delay(pollInterval);
                waited += pollInterval;
                continue;
            }
            size_t chunk = std::min(space, data.size());
            TRY(bus->writeBurst(subaddress(REG_THR, channel), reinterpret_cast<const uint8_t*>(data.data()), chunk));
            data.remove_prefix(chunk);
        }
        return {};
    }

    void sleepFor(std::chrono::milliseconds duration) const {
        delay(duration);
    }

private:
    static std::string toHex(uint8_t value) {
        static constexpr char DIGITS[] = "0123456789ABCDEF";
        return { DIGITS[value >> 4], DIGITS[value & 0x0F] };
    }

    const std::unique_ptr<ISc16is752Bus> bus;
    const uint32_t crystalHz;
    const DelayFunction delay;

    // Shadow of IOState's outputs, so one pin can change without a read-modify-write over I²C
    uint8_t ioState = 0;
};

// One SC16IS752 channel as a line-oriented serial port. Polls RXLVL every `pollInterval`.
// Timeouts count the time spent waiting between polls, not the I²C transactions themselves, so they
// run slightly long; that's fine for the command / response timeouts they guard.
class Sc16is752Port : public ISerialPort {
public:
    Sc16is752Port(Sc16is752& chip, Sc16is752::Channel channel, std::chrono::milliseconds pollInterval)
        : chip(chip)
        , channel(channel)
        , pollInterval(pollInterval) {
    }

    Result<> write(std::string_view data) override {
        // At 9600 baud a full 64-byte FIFO drains in ~67 ms
        return chip.write(channel, data, std::chrono::milliseconds(200), pollInterval);
    }

    Result<std::string> readUntil(char terminator, size_t maxLength, std::chrono::milliseconds timeout) override {
        auto waited = std::chrono::milliseconds::zero();
        while (true) {
            if (auto line = takeLine(terminator)) {
                return std::move(*line);
            }
            if (pending.size() >= maxLength) {
                auto received = std::string(pending.begin(), pending.end());
                pending.clear();
                return failure("no terminator within " + std::to_string(maxLength) + " bytes, received: '" + escape(received) + "'");
            }
            size_t available = TRY(chip.rxLevel(channel));
            if (available > 0) {
                size_t start = pending.size();
                pending.resize(start + available);
                TRY(chip.readFifo(channel, pending.data() + start, available));
                continue;
            }
            if (waited >= timeout) {
                auto received = std::string(pending.begin(), pending.end());
                pending.clear();
                return failure(received.empty()
                        ? "no response within " + std::to_string(timeout.count()) + " ms"
                        : "incomplete response within " + std::to_string(timeout.count()) + " ms: '" + escape(received) + "'");
            }
            chip.sleepFor(pollInterval);
            waited += pollInterval;
        }
    }

    Result<std::string> read(size_t length, std::chrono::milliseconds timeout) override {
        auto waited = std::chrono::milliseconds::zero();
        while (pending.size() < length) {
            size_t available = TRY(chip.rxLevel(channel));
            if (available > 0) {
                size_t start = pending.size();
                pending.resize(start + available);
                TRY(chip.readFifo(channel, pending.data() + start, available));
                continue;
            }
            if (waited >= timeout) {
                auto received = std::string(pending.begin(), pending.end());
                pending.clear();
                return failure(received.empty()
                        ? "no response within " + std::to_string(timeout.count()) + " ms"
                        : "only " + std::to_string(received.size()) + " of " + std::to_string(length)
                            + " bytes within " + std::to_string(timeout.count()) + " ms: '" + escape(received) + "'");
            }
            chip.sleepFor(pollInterval);
            waited += pollInterval;
        }
        std::string data(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(length));
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(length));
        return data;
    }

    Result<> flushRx() override {
        pending.clear();
        // Drain rather than reset the FIFO: a reset would also drop anything queued for TX
        std::array<uint8_t, Sc16is752::FIFO_SIZE> scratch {};
        while (true) {
            size_t available = TRY(chip.rxLevel(channel));
            if (available == 0) {
                return {};
            }
            TRY(chip.readFifo(channel, scratch.data(), available));
        }
    }

    // Printable form of received bytes for log and error messages
    static std::string escape(std::string_view data) {
        std::string result;
        for (char c : data) {
            auto byte = static_cast<uint8_t>(c);
            if (c == '\r') {
                result += "\\r";
            } else if (c == '\n') {
                result += "\\n";
            } else if (byte < 0x20 || byte >= 0x7F) {
                static constexpr char DIGITS[] = "0123456789ABCDEF";
                result += "\\x";
                result += DIGITS[byte >> 4];
                result += DIGITS[byte & 0x0F];
            } else {
                result += c;
            }
        }
        return result;
    }

private:
    std::optional<std::string> takeLine(char terminator) {
        auto it = std::ranges::find(pending, static_cast<uint8_t>(terminator));
        if (it == pending.end()) {
            return std::nullopt;
        }
        std::string line(pending.begin(), it);
        pending.erase(pending.begin(), it + 1);
        return line;
    }

    Sc16is752& chip;
    const Sc16is752::Channel channel;
    const std::chrono::milliseconds pollInterval;

    // Received bytes not yet returned; may hold the start of the next line
    std::vector<uint8_t> pending;
};

}    // namespace cornucopia::ugly_duckling::peripherals::serial
