#pragma once

/*
 * Register map of the ZTS-3002-TR-ECTHNPK(PH)-N01 soil multi-parameter sensor (RS485, Modbus RTU),
 * from datasheets/zts_soil_ecthnpkph_n01.pdf and datasheets/notes/zts_soil_ecthnpkph_n01.md.
 *
 * Pure C++: no ESP-IDF headers, so this compiles in native unit tests.
 */

#include <Log.hpp>
#include <Result.hpp>
#include <peripherals/serial/ModbusRtu.hpp>
#include <peripherals/serial/SerialPort.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace cornucopia::ugly_duckling::peripherals::environment {

using cornucopia::ugly_duckling::kernel::Result;
using cornucopia::ugly_duckling::peripherals::serial::ISerialPort;
using cornucopia::ugly_duckling::peripherals::serial::ModbusRtu;

LOGGING_TAG(ZTS_SOIL, "zts-soil")

struct ZtsSoilReading {
    static constexpr double NOT_READ = std::numeric_limits<double>::quiet_NaN();

    double moisture = NOT_READ;        // volumetric water content, %
    double temperature = NOT_READ;     // °C
    double conductivity = NOT_READ;    // µS/cm
    // Not measured: until calibrated by writing to them, the sensor derives these from conductivity
    double nitrogen = NOT_READ;      // mg/kg
    double phosphorus = NOT_READ;    // mg/kg
    double potassium = NOT_READ;     // mg/kg
};

class ZtsSoilProtocol {
public:
    static constexpr uint8_t DEFAULT_ADDRESS = 1;
    static constexpr uint32_t DEFAULT_BAUD = 4800;

    // Moisture (0x0000), temperature (0x0001), conductivity (0x0002)
    static constexpr uint16_t BASIC_REGISTERS = 3;
    // ... then pH (0x0003, unused on the variants without pH) and N, P, K (0x0004 to 0x0006)
    static constexpr uint16_t NPK_REGISTERS = 7;

    ZtsSoilProtocol(uint8_t address, bool readNpk, std::chrono::milliseconds responseTimeout)
        : address(address)
        , readNpk(readNpk)
        , modbus(responseTimeout) {
    }

    Result<ZtsSoilReading> read(ISerialPort& port) {
        auto registers = TRY(modbus.readHoldingRegisters(port, address, 0x0000, readNpk ? NPK_REGISTERS : BASIC_REGISTERS));
        return decode(registers);
    }

    static ZtsSoilReading decode(const std::vector<uint16_t>& registers) {
        ZtsSoilReading reading;
        if (registers.size() >= BASIC_REGISTERS) {
            reading.moisture = registers[0] / 10.0;
            // Two's complement below 0 °C
            reading.temperature = static_cast<int16_t>(registers[1]) / 10.0;
            reading.conductivity = registers[2];
        }
        if (registers.size() >= NPK_REGISTERS) {
            reading.nitrogen = registers[4];
            reading.phosphorus = registers[5];
            reading.potassium = registers[6];
        }
        return reading;
    }

    std::string describe() const {
        return "Modbus address " + std::to_string(address);
    }

private:
    const uint8_t address;
    const bool readNpk;
    ModbusRtu modbus;
};

}    // namespace cornucopia::ugly_duckling::peripherals::environment
