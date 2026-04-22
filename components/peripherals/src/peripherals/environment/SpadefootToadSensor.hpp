#pragma once

#include <limits>
#include <memory>
#include <stdexcept>

#include <I2CManager.hpp>
#include <Task.hpp>

#include <peripherals/I2CSettings.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/api/ISoilMoistureSensor.hpp>
#include <peripherals/api/ITemperatureSensor.hpp>

#include <utils/DebouncedMeasurement.hpp>

#include "Environment.hpp"

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::peripherals;

namespace cornucopia::ugly_duckling::peripherals::environment {

class SpadefootToadSensorSettings
    : public I2CSettings {
    // No extra fields; I2C address comes from I2CSettings.
};

struct SpadefootToadReading {
    double moisture = std::numeric_limits<double>::quiet_NaN();
    double temperature = std::numeric_limits<double>::quiet_NaN();
};

class SpadefootToadSensor final
    : public api::ISoilMoistureSensor,
      public api::ITemperatureSensor,
      public Peripheral {
public:
    SpadefootToadSensor(
        const std::string& name,
        const std::shared_ptr<I2CManager>& i2c,
        const I2CConfig& config)
        : Peripheral(name)
        , device(i2c->createDevice(name, config)) {

        LOGTI(ENV, "Initializing Spadefoot Toad soil sensor '%s' with %s",
            name.c_str(), config.toString().c_str());

        // Verify manufacturer ID
        uint16_t mfrId = __builtin_bswap16(device->readRegWord(CMD_GET_MFR_ID));
        if (mfrId != EXPECTED_MFR_ID) {
            LOGTE(ENV, "Spadefoot Toad '%s': unexpected manufacturer ID 0x%04X (expected 0x%04X)",
                name.c_str(), mfrId, EXPECTED_MFR_ID);
            throw std::runtime_error("Spadefoot Toad: manufacturer ID mismatch");
        }

        // Verify device ID
        uint16_t deviceId = __builtin_bswap16(device->readRegWord(CMD_GET_DEVICE_ID));
        if (deviceId != EXPECTED_DEVICE_ID) {
            LOGTE(ENV, "Spadefoot Toad '%s': unexpected device ID 0x%04X (expected 0x%04X)",
                name.c_str(), deviceId, EXPECTED_DEVICE_ID);
            throw std::runtime_error("Spadefoot Toad: device ID mismatch");
        }

        // Log firmware and device revisions
        uint16_t firmwareRev = __builtin_bswap16(device->readRegWord(CMD_GET_FIRMWARE_REV));
        LOGTI(ENV, "Spadefoot Toad firmware rev: 0x%04X", firmwareRev);

        uint16_t deviceRev = __builtin_bswap16(device->readRegWord(CMD_GET_DEVICE_REV));
        LOGTI(ENV, "Spadefoot Toad device rev: 0x%04X", deviceRev);
    }

    Percent getMoisture() override {
        return measurement.getValue().moisture;
    }

    Celsius getTemperature() override {
        return measurement.getValue().temperature;
    }

private:
    static constexpr uint8_t CMD_TRIGGER = 0x01;
    static constexpr uint8_t CMD_READ = 0x02;
    static constexpr uint8_t CMD_GET_FIRMWARE_REV = 0xFC;
    static constexpr uint8_t CMD_GET_DEVICE_REV = 0xFD;
    static constexpr uint8_t CMD_GET_MFR_ID = 0xFE;
    static constexpr uint8_t CMD_GET_DEVICE_ID = 0xFF;

    static constexpr uint16_t EXPECTED_MFR_ID = 0x434D;
    static constexpr uint16_t EXPECTED_DEVICE_ID = 0x0010;

    static constexpr uint8_t FLAG_MOISTURE_VALID = 0x01;
    static constexpr uint8_t FLAG_TEMPERATURE_VALID = 0x02;
    static constexpr uint8_t MOISTURE_INVALID = 0xFF;
    static constexpr int16_t TEMP_INVALID = INT16_MIN;

    const std::shared_ptr<I2CDevice> device;

    utils::DebouncedMeasurement<SpadefootToadReading> measurement {
        [this](const utils::DebouncedParams<SpadefootToadReading>) -> std::optional<SpadefootToadReading> {
            // Phase 1: trigger measurement
            device->writeByte(CMD_TRIGGER);

            // Phase 2: wait 1 s, then read results
            Task::delay(1s);
            auto raw = device->readBytes(CMD_READ, 10);

            // Validate checksum (XOR of bytes [0..8])
            uint8_t csum = 0;
            for (int i = 0; i < 9; i++) {
                csum ^= raw[i];
            }
            if (csum != raw[9]) {
                LOGTW(ENV, "Spadefoot Toad '%s': checksum mismatch", getName().c_str());
                return std::nullopt;
            }

            SpadefootToadReading reading;
            uint8_t flags = raw[8];

            // Moisture: average of valid probes (flag bit 0 set and value != 0xFF)
            if (flags & FLAG_MOISTURE_VALID) {
                int sum = 0, count = 0;
                for (int i = 0; i < 4; i++) {
                    if (raw[i] != MOISTURE_INVALID) {
                        sum += raw[i];
                        count++;
                    }
                }
                if (count > 0) {
                    reading.moisture = static_cast<double>(sum) / count;
                }
                LOGTV(ENV, "Spadefoot Toad '%s': moisture TF=%d TR=%d BF=%d BR=%d → avg=%.1f%%",
                    getName().c_str(), raw[0], raw[1], raw[2], raw[3], reading.moisture);
            }

            // Temperature: average of top and bottom.
            // Requires the temperature valid flag AND both individual readings to be valid.
            if (flags & FLAG_TEMPERATURE_VALID) {
                auto temp_top = static_cast<int16_t>((raw[4] << 8) | raw[5]);
                auto temp_bot = static_cast<int16_t>((raw[6] << 8) | raw[7]);
                if (temp_top != TEMP_INVALID && temp_bot != TEMP_INVALID) {
                    reading.temperature = (temp_top + temp_bot) / 20.0;    // two sensors, 0.1 °C units
                    LOGTV(ENV, "Spadefoot Toad '%s': temp top=%.1f°C bot=%.1f°C → avg=%.1f°C",
                        getName().c_str(), temp_top / 10.0, temp_bot / 10.0, reading.temperature);
                } else {
                    LOGTW(ENV, "Spadefoot Toad '%s': temperature valid flag set but individual reading(s) invalid (top=%d bot=%d)",
                        getName().c_str(), temp_top, temp_bot);
                }
            }

            return reading;
        },
        2s
    };
};

inline PeripheralFactory makeFactoryForSpadefootToadSensor() {
    return makePeripheralFactory<
        SpadefootToadSensor,
        SpadefootToadSensorSettings,
        api::ISoilMoistureSensor,
        api::ITemperatureSensor>(
        "soil:spadefoot-toad",
        "environment",
        [](PeripheralInitParameters& params, const std::shared_ptr<SpadefootToadSensorSettings>& settings) {
            I2CConfig i2cConfig = settings->parse(0x20);
            i2cConfig.clkSpeed = 100000;    // ATtiny TWI Address Match wakeup requires 100 kHz
            auto sensor = std::make_shared<SpadefootToadSensor>(
                params.name,
                params.services.i2c,
                i2cConfig);
            params.registerFeature("moisture", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getMoisture();
            });
            params.registerFeature("temperature", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getTemperature();
            });
            return sensor;
        });
}

}    // namespace cornucopia::ugly_duckling::peripherals::environment
