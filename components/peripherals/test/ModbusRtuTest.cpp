#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <peripherals/environment/ZtsSoilProtocol.hpp>
#include <peripherals/serial/ModbusRtu.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using namespace cornucopia::ugly_duckling::peripherals::environment;
using namespace cornucopia::ugly_duckling::peripherals::serial;
using cornucopia::ugly_duckling::kernel::failure;
using cornucopia::ugly_duckling::kernel::Result;

namespace {

std::string bytes(std::initializer_list<uint8_t> values) {
    std::string result;
    for (auto value : values) {
        result += static_cast<char>(value);
    }
    return result;
}

// A byte stream: answers each write with whatever `respond` returns, and serves it to read()
class FakeBinaryPort : public ISerialPort {
public:
    Result<> write(std::string_view data) override {
        written.emplace_back(data);
        if (respond) {
            rx += respond(std::string(data));
        }
        return {};
    }

    Result<std::string> readUntil(char /*terminator*/, size_t /*maxLength*/, std::chrono::milliseconds /*timeout*/) override {
        return failure("not used by Modbus");
    }

    Result<std::string> read(size_t length, std::chrono::milliseconds timeout) override {
        if (rx.size() < length) {
            rx.clear();
            return failure("no response within " + std::to_string(timeout.count()) + " ms");
        }
        auto data = rx.substr(0, length);
        rx.erase(0, length);
        return data;
    }

    Result<> flushRx() override {
        rx.clear();
        flushes++;
        return {};
    }

    std::function<std::string(const std::string&)> respond;
    std::vector<std::string> written;
    std::string rx;
    int flushes = 0;
};

constexpr auto TIMEOUT = std::chrono::milliseconds(100);

}    // namespace

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

TEST_CASE("Modbus CRC matches the ZTS manual's example frames") {
    // Read 4 registers from 0x0000 at address 1
    CHECK(ModbusRtu::withCrc(bytes({ 0x01, 0x03, 0x00, 0x00, 0x00, 0x04 })) == bytes({ 0x01, 0x03, 0x00, 0x00, 0x00, 0x04, 0x44, 0x09 }));
    // Change the address from 1 to 2
    CHECK(ModbusRtu::withCrc(bytes({ 0x01, 0x06, 0x07, 0xD0, 0x00, 0x02 })) == bytes({ 0x01, 0x06, 0x07, 0xD0, 0x00, 0x02, 0x08, 0x86 }));
    // The manual's response to the 4-register read
    CHECK(ModbusRtu::withCrc(bytes({ 0x01, 0x03, 0x08, 0x02, 0x92, 0xFF, 0x9B, 0x03, 0xE8, 0x00, 0x38 }))
        == bytes({ 0x01, 0x03, 0x08, 0x02, 0x92, 0xFF, 0x9B, 0x03, 0xE8, 0x00, 0x38, 0x57, 0xB6 }));
}

TEST_CASE("Modbus read request for 8 registers") {
    CHECK(ModbusRtu::readHoldingRegistersRequest(1, 0x0000, 8) == bytes({ 0x01, 0x03, 0x00, 0x00, 0x00, 0x08, 0x44, 0x0C }));
}

// ---------------------------------------------------------------------------
// Reading registers
// ---------------------------------------------------------------------------

TEST_CASE("Modbus reads holding registers") {
    FakeBinaryPort port;
    port.respond = [](const std::string&) {
        return ModbusRtu::withCrc(bytes({ 0x01, 0x03, 0x04, 0x02, 0x92, 0xFF, 0x9B }));
    };
    ModbusRtu modbus(TIMEOUT);

    auto registers = modbus.readHoldingRegisters(port, 1, 0x0000, 2);

    REQUIRE(registers.has_value());
    CHECK(*registers == std::vector<uint16_t> { 0x0292, 0xFF9B });
    REQUIRE(port.written.size() == 1);
    CHECK(port.written[0] == ModbusRtu::readHoldingRegistersRequest(1, 0x0000, 2));
    CHECK(port.flushes == 1);
}

TEST_CASE("Modbus rejects a response with a bad CRC") {
    FakeBinaryPort port;
    port.respond = [](const std::string&) {
        auto frame = ModbusRtu::withCrc(bytes({ 0x01, 0x03, 0x02, 0x00, 0x01 }));
        frame.back() = static_cast<char>(frame.back() ^ 0x01);
        return frame;
    };
    ModbusRtu modbus(TIMEOUT);

    auto registers = modbus.readHoldingRegisters(port, 1, 0x0000, 1);

    REQUIRE_FALSE(registers.has_value());
    CHECK(registers.error().message.find("CRC") != std::string::npos);
}

TEST_CASE("Modbus reports an exception response") {
    FakeBinaryPort port;
    port.respond = [](const std::string&) {
        return ModbusRtu::withCrc(bytes({ 0x01, 0x83, 0x02 }));
    };
    ModbusRtu modbus(TIMEOUT);

    auto registers = modbus.readHoldingRegisters(port, 1, 0x0100, 1);

    REQUIRE_FALSE(registers.has_value());
    CHECK(registers.error().message.find("illegal data address") != std::string::npos);
    CHECK(registers.error().message.find("0x0100") != std::string::npos);
}

TEST_CASE("Modbus rejects a response from another address") {
    FakeBinaryPort port;
    port.respond = [](const std::string&) {
        return ModbusRtu::withCrc(bytes({ 0x02, 0x03, 0x02, 0x00, 0x01 }));
    };
    ModbusRtu modbus(TIMEOUT);

    auto registers = modbus.readHoldingRegisters(port, 1, 0x0000, 1);

    REQUIRE_FALSE(registers.has_value());
    CHECK(registers.error().message.find("address 2") != std::string::npos);
}

TEST_CASE("Modbus rejects a response with the wrong byte count") {
    FakeBinaryPort port;
    port.respond = [](const std::string&) {
        return ModbusRtu::withCrc(bytes({ 0x01, 0x03, 0x02, 0x00, 0x01 }));
    };
    ModbusRtu modbus(TIMEOUT);

    auto registers = modbus.readHoldingRegisters(port, 1, 0x0000, 2);

    REQUIRE_FALSE(registers.has_value());
    CHECK(registers.error().message.find("expected 4 data bytes") != std::string::npos);
}

TEST_CASE("Modbus fails when the slave doesn't answer") {
    FakeBinaryPort port;
    ModbusRtu modbus(TIMEOUT);

    auto registers = modbus.readHoldingRegisters(port, 1, 0x0000, 1);

    REQUIRE_FALSE(registers.has_value());
    CHECK(registers.error().message.find("no response") != std::string::npos);
}

TEST_CASE("Modbus refuses invalid addresses and register counts") {
    FakeBinaryPort port;
    ModbusRtu modbus(TIMEOUT);

    CHECK_FALSE(modbus.readHoldingRegisters(port, 0, 0x0000, 1).has_value());
    CHECK_FALSE(modbus.readHoldingRegisters(port, 248, 0x0000, 1).has_value());
    CHECK_FALSE(modbus.readHoldingRegisters(port, 1, 0x0000, 0).has_value());
    CHECK_FALSE(modbus.readHoldingRegisters(port, 1, 0x0000, 126).has_value());
    CHECK(port.written.empty());
}

// ---------------------------------------------------------------------------
// ZTS soil sensor
// ---------------------------------------------------------------------------

TEST_CASE("ZTS soil sensor decodes the manual's example reading") {
    auto reading = ZtsSoilProtocol::decode({ 0x0292, 0xFF9B, 0x03E8, 0x0038 });

    CHECK(reading.moisture == Catch::Approx(65.8));
    CHECK(reading.temperature == Catch::Approx(-10.1));
    CHECK(reading.conductivity == Catch::Approx(1000));
    CHECK(std::isnan(reading.nitrogen));
    CHECK(std::isnan(reading.phosphorus));
    CHECK(std::isnan(reading.potassium));
}

TEST_CASE("ZTS soil sensor reads only moisture, temperature and conductivity by default") {
    FakeBinaryPort port;
    port.respond = [](const std::string&) {
        return ModbusRtu::withCrc(bytes({ 0x01, 0x03, 0x06, 0x00, 0xDD, 0x00, 0xF5, 0x00, 0x8C }));
    };
    ZtsSoilProtocol protocol(1, false, TIMEOUT);

    auto reading = protocol.read(port);

    REQUIRE(reading.has_value());
    CHECK(port.written[0] == ModbusRtu::readHoldingRegistersRequest(1, 0x0000, 3));
    CHECK(reading->moisture == Catch::Approx(22.1));
    CHECK(reading->temperature == Catch::Approx(24.5));
    CHECK(reading->conductivity == Catch::Approx(140));
    CHECK(std::isnan(reading->nitrogen));
}

TEST_CASE("ZTS soil sensor reads N, P and K when asked to") {
    FakeBinaryPort port;
    port.respond = [](const std::string&) {
        return ModbusRtu::withCrc(bytes({ 0x01, 0x03, 0x0E,
            0x00, 0xDD,        // moisture 22.1 %
            0x00, 0xF5,        // temperature 24.5 °C
            0x00, 0x8C,        // conductivity 140 µS/cm
            0x00, 0x00,        // no pH on this variant
            0x00, 0x0B,        // nitrogen 11 mg/kg
            0x00, 0x0F,        // phosphorus 15 mg/kg
            0x00, 0x24 }));    // potassium 36 mg/kg
    };
    ZtsSoilProtocol protocol(1, true, TIMEOUT);

    auto reading = protocol.read(port);

    REQUIRE(reading.has_value());
    CHECK(port.written[0] == ModbusRtu::readHoldingRegistersRequest(1, 0x0000, 7));
    CHECK(reading->nitrogen == Catch::Approx(11));
    CHECK(reading->phosphorus == Catch::Approx(15));
    CHECK(reading->potassium == Catch::Approx(36));
}
