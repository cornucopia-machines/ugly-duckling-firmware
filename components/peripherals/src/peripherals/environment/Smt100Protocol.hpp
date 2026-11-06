#pragma once

/*
 * Protocols for talking to a Truebner SMT100 soil probe over a serial line.
 *
 * The SMT100 speaks ASCII (TRUEBNER AN005) by default, and can be switched to Modbus RTU with
 * Truebner's tool. ISmt100Protocol keeps the sensor peripheral independent of that choice; only
 * ASCII is implemented for now.
 *
 * Pure C++: no ESP-IDF headers, so this compiles in native unit tests.
 */

#include <Log.hpp>
#include <Result.hpp>
#include <peripherals/serial/SerialPort.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace cornucopia::ugly_duckling::peripherals::environment {

using cornucopia::ugly_duckling::kernel::failure;
using cornucopia::ugly_duckling::kernel::Result;
using cornucopia::ugly_duckling::peripherals::serial::ISerialPort;

LOGGING_TAG(SMT100, "smt100")

struct Smt100Reading {
    double moisture = std::numeric_limits<double>::quiet_NaN();       // volumetric water content, %
    double temperature = std::numeric_limits<double>::quiet_NaN();    // °C
};

struct ISmt100Protocol {
    virtual ~ISmt100Protocol() = default;

    // Reads water content and temperature from a powered, started-up probe
    virtual Result<Smt100Reading> read(ISerialPort& port) = 0;

    // A human-readable identification of the probe (e.g. its firmware version)
    virtual Result<std::string> identify(ISerialPort& port) = 0;

    virtual std::string describe() const = 0;
};

// ASCII protocol (AN005, AN007): 9600 8N1, "<Command>!<address>" terminated by CR, answered with the
// value terminated by CR+LF, e.g. "GetTemperature!000000\r" -> "24.6\r\n".
// Address 000000 is the broadcast address, valid while only one probe shares the line.
class Smt100AsciiProtocol final : public ISmt100Protocol {
public:
    static constexpr const char* BROADCAST_ADDRESS = "000000";
    static constexpr size_t MAX_RESPONSE_LENGTH = 32;

    static Result<std::unique_ptr<Smt100AsciiProtocol>> create(const std::string& address, std::chrono::milliseconds responseTimeout) {
        TRY(validateAddress(address));
        return std::unique_ptr<Smt100AsciiProtocol>(new Smt100AsciiProtocol(address, responseTimeout));
    }

    // Addresses are up to 6 decimal digits (AN005)
    static Result<> validateAddress(std::string_view address) {
        if (address.empty() || address.size() > 6) {
            return failure("SMT100 address must be 1 to 6 digits, got '" + std::string(address) + "'");
        }
        for (char c : address) {
            if (c < '0' || c > '9') {
                return failure("SMT100 address must be 1 to 6 digits, got '" + std::string(address) + "'");
            }
        }
        return {};
    }

    // Parses a value response, with the LF already removed. Tolerates the CR and surrounding spaces.
    static Result<double> parseValue(std::string_view response) {
        auto trimmed = trim(response);
        if (trimmed.empty()) {
            return failure("empty response");
        }
        std::string text(trimmed);
        char* end = nullptr;
        double value = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size() || !std::isfinite(value)) {
            return failure("not a number: '" + text + "'");
        }
        return value;
    }

    Result<Smt100Reading> read(ISerialPort& port) override {
        Smt100Reading reading;
        reading.moisture = TRY(queryValue(port, "GetWaterContent"));
        reading.temperature = TRY(queryValue(port, "GetTemperature"));
        return reading;
    }

    Result<std::string> identify(ISerialPort& port) override {
        auto response = TRY(query(port, "GetFirmwareVersion"));
        return std::string(trim(response));
    }

    std::string describe() const override {
        return "ASCII, address " + address;
    }

    // Sends one command and returns the raw response line without its LF
    Result<std::string> query(ISerialPort& port, std::string_view command) {
        TRY(port.flushRx());
        std::string request = std::string(command) + "!" + address + "\r";
        auto start = std::chrono::steady_clock::now();
        TRY(port.write(request));
        auto response = port.readUntil('\n', MAX_RESPONSE_LENGTH, responseTimeout);
        [[maybe_unused]] auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        if (!response) {
            return failure(std::string(command) + ": " + response.error().message);
        }
        LOGTD(SMT100, "%.*s -> '%.*s' in %lld ms",
            static_cast<int>(command.size()), command.data(),
            static_cast<int>(trim(*response).size()), trim(*response).data(),
            static_cast<long long>(elapsed.count()));
        return response;
    }

private:
    Smt100AsciiProtocol(std::string address, std::chrono::milliseconds responseTimeout)
        : address(std::move(address))
        , responseTimeout(responseTimeout) {
    }

    Result<double> queryValue(ISerialPort& port, std::string_view command) {
        auto response = TRY(query(port, command));
        auto value = parseValue(response);
        if (!value) {
            return failure(std::string(command) + ": " + value.error().message);
        }
        return *value;
    }

    static std::string_view trim(std::string_view text) {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\r' || text.front() == '\n')) {
            text.remove_prefix(1);
        }
        while (!text.empty() && (text.back() == ' ' || text.back() == '\r' || text.back() == '\n')) {
            text.remove_suffix(1);
        }
        return text;
    }

    const std::string address;
    const std::chrono::milliseconds responseTimeout;
};

}    // namespace cornucopia::ugly_duckling::peripherals::environment
