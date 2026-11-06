#pragma once

/*
 * Modbus RTU master: just enough to read holding registers from a sensor on a serial line.
 *
 * A frame is <address> <function> <data...> <CRC-16, low byte first>. Frames are delimited by
 * silence on the line, but every response we expect carries its own length, so we read it by length
 * (ISerialPort::read) and never have to time the 3.5-character gaps ourselves.
 *
 * Pure C++: no ESP-IDF headers, so this compiles in native unit tests.
 */

#include "SerialPort.hpp"
#include <Log.hpp>
#include <Result.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cornucopia::ugly_duckling::peripherals::serial {

using cornucopia::ugly_duckling::kernel::failure;
using cornucopia::ugly_duckling::kernel::Result;

LOGGING_TAG(MODBUS, "modbus")

class ModbusRtu {
public:
    static constexpr uint8_t READ_HOLDING_REGISTERS = 0x03;
    static constexpr uint8_t EXCEPTION_FLAG = 0x80;
    // A response's byte count is one byte, so at most 125 registers fit in one (Modbus spec)
    static constexpr uint16_t MAX_READ_REGISTERS = 125;

    // CRC-16/MODBUS: polynomial 0xA001 (reflected 0x8005), initial value 0xFFFF
    static uint16_t crc16(std::string_view data) {
        uint16_t crc = 0xFFFF;
        for (char c : data) {
            crc ^= static_cast<uint8_t>(c);
            for (int bit = 0; bit < 8; bit++) {
                crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xA001 : crc >> 1;
            }
        }
        return crc;
    }

    // Appends the CRC, low byte first
    static std::string withCrc(std::string frame) {
        uint16_t crc = crc16(frame);
        frame += static_cast<char>(crc & 0xFF);
        frame += static_cast<char>(crc >> 8);
        return frame;
    }

    static std::string readHoldingRegistersRequest(uint8_t address, uint16_t start, uint16_t count) {
        std::string frame;
        frame += static_cast<char>(address);
        frame += static_cast<char>(READ_HOLDING_REGISTERS);
        frame += static_cast<char>(start >> 8);
        frame += static_cast<char>(start & 0xFF);
        frame += static_cast<char>(count >> 8);
        frame += static_cast<char>(count & 0xFF);
        return withCrc(std::move(frame));
    }

    explicit ModbusRtu(std::chrono::milliseconds responseTimeout)
        : responseTimeout(responseTimeout) {
    }

    // Reads `count` consecutive holding registers (function 0x03) from the slave at `address`
    Result<std::vector<uint16_t>> readHoldingRegisters(ISerialPort& port, uint8_t address, uint16_t start, uint16_t count) {
        if (address == 0 || address > 247) {
            return failure("Modbus address must be 1 to 247, got " + std::to_string(address));
        }
        if (count == 0 || count > MAX_READ_REGISTERS) {
            return failure("Modbus can read 1 to 125 registers at once, asked for " + std::to_string(count));
        }

        TRY(port.flushRx());
        auto sentAt = std::chrono::steady_clock::now();
        TRY(port.write(readHoldingRegistersRequest(address, start, count)));

        // Address, function, and either the byte count or the exception code
        auto header = TRY(readPart(port, 3, address, start));
        if (static_cast<uint8_t>(header[0]) != address) {
            return failure(describe(address, start) + ": response from address " + std::to_string(static_cast<uint8_t>(header[0])));
        }
        auto function = static_cast<uint8_t>(header[1]);
        if (function == (READ_HOLDING_REGISTERS | EXCEPTION_FLAG)) {
            auto crc = TRY(readPart(port, 2, address, start));
            TRY(checkCrc(header + crc, address, start));
            return failure(describe(address, start) + ": exception " + exceptionName(static_cast<uint8_t>(header[2])));
        }
        if (function != READ_HOLDING_REGISTERS) {
            return failure(describe(address, start) + ": unexpected function code " + std::to_string(function));
        }
        auto byteCount = static_cast<uint8_t>(header[2]);
        if (byteCount != count * 2) {
            return failure(describe(address, start) + ": expected " + std::to_string(count * 2)
                + " data bytes, response announces " + std::to_string(byteCount));
        }

        auto rest = TRY(readPart(port, byteCount + 2, address, start));
        auto frame = header + rest;
        TRY(checkCrc(frame, address, start));

        std::vector<uint16_t> registers;
        registers.reserve(count);
        for (size_t i = 0; i < count; i++) {
            auto high = static_cast<uint8_t>(frame[3 + (2 * i)]);
            auto low = static_cast<uint8_t>(frame[4 + (2 * i)]);
            registers.push_back(static_cast<uint16_t>((high << 8) | low));
        }
        [[maybe_unused]] auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sentAt);
        LOGTD(MODBUS, "Read %u register(s) from 0x%04X at address %u in %lld ms",
            count, start, address, static_cast<long long>(elapsed.count()));
        return registers;
    }

    static std::string exceptionName(uint8_t code) {
        switch (code) {
            case 0x01:
                return "illegal function (0x01)";
            case 0x02:
                return "illegal data address (0x02)";
            case 0x03:
                return "illegal data value (0x03)";
            case 0x04:
                return "slave device failure (0x04)";
            default:
                return "code " + std::to_string(code);
        }
    }

private:
    Result<std::string> readPart(ISerialPort& port, size_t length, uint8_t address, uint16_t start) {
        auto part = port.read(length, responseTimeout);
        if (!part) {
            return failure(describe(address, start) + ": " + part.error().message);
        }
        return part;
    }

    static Result<> checkCrc(std::string_view frame, uint8_t address, uint16_t start) {
        auto payload = frame.substr(0, frame.size() - 2);
        uint16_t expected = crc16(payload);
        auto received = static_cast<uint16_t>(static_cast<uint8_t>(frame[frame.size() - 2]) | (static_cast<uint8_t>(frame[frame.size() - 1]) << 8));
        if (expected != received) {
            return failure(describe(address, start) + ": CRC mismatch");
        }
        return {};
    }

    static std::string describe(uint8_t address, uint16_t start) {
        static constexpr char DIGITS[] = "0123456789ABCDEF";
        std::string reg = "0x";
        for (int shift = 12; shift >= 0; shift -= 4) {
            reg += DIGITS[(start >> shift) & 0xF];
        }
        return "Modbus read of " + reg + " at address " + std::to_string(address);
    }

    const std::chrono::milliseconds responseTimeout;
};

}    // namespace cornucopia::ugly_duckling::peripherals::serial
