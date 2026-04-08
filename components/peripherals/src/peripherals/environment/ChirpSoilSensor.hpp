#pragma once

#include <limits>
#include <memory>

#include <I2CManager.hpp>

#include <peripherals/I2CSettings.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/api/ISoilMoistureSensor.hpp>
#include <peripherals/api/ITemperatureSensor.hpp>

#include <utils/DebouncedMeasurement.hpp>

#include "Environment.hpp"

using namespace farmhub::kernel;
using namespace farmhub::peripherals;

namespace farmhub::peripherals::environment {

class ChirpSoilSensorSettings
    : public I2CSettings {
public:
    // Raw capacitance calibration values
    Property<uint16_t> air { this, "air", 250 };
    Property<uint16_t> water { this, "water", 550 };
};

struct ChirpReading {
    double moisture = std::numeric_limits<double>::quiet_NaN();
    double temperature = std::numeric_limits<double>::quiet_NaN();
};

class ChirpSoilSensor final
    : public api::ISoilMoistureSensor,
      public api::ITemperatureSensor,
      public Peripheral {
public:
    ChirpSoilSensor(
        const std::string& name,
        const std::shared_ptr<I2CManager>& i2c,
        const I2CConfig& config,
        int airValue,
        int waterValue)
        : Peripheral(name)
        , device(i2c->createDevice(name, config))
        , airValue(airValue)
        , waterValue(waterValue) {

        LOGTI(ENV, "Initializing Chirp soil sensor '%s' with %s; air value: %d; water value: %d",
            name.c_str(), config.toString().c_str(), airValue, waterValue);
    }

    Percent getMoisture() override {
        return measurement.getValue().moisture;
    }

    Celsius getTemperature() override {
        return measurement.getValue().temperature;
    }

private:
    static constexpr uint8_t REG_CAPACITANCE = 0x00;
    static constexpr uint8_t REG_TEMPERATURE = 0x05;

    const std::shared_ptr<I2CDevice> device;
    const int airValue;
    const int waterValue;

    utils::DebouncedMeasurement<ChirpReading> measurement {
        [this](const utils::DebouncedParams<ChirpReading>) -> std::optional<ChirpReading> {
            ChirpReading reading;

            try {
                // Capacitance: 16-bit big-endian
                uint16_t rawMoisture = device->readRegWord(REG_CAPACITANCE);
                rawMoisture = __builtin_bswap16(rawMoisture);

                double run = waterValue - airValue;
                reading.moisture = ((rawMoisture - airValue) * 100.0) / run;

                LOGTV(ENV, "Chirp capacitance: %d, moisture: %.1f%%",
                    rawMoisture, reading.moisture);
            } catch (const std::exception& e) {
                LOGTW(ENV, "Failed to read Chirp capacitance: %s", e.what());
            }

            try {
                // Temperature: 16-bit big-endian, units of 1/10 °C
                uint16_t rawTemp = device->readRegWord(REG_TEMPERATURE);
                rawTemp = __builtin_bswap16(rawTemp);
                reading.temperature = rawTemp / 10.0;

                LOGTV(ENV, "Chirp temperature: %.1f°C", reading.temperature);
            } catch (const std::exception& e) {
                LOGTW(ENV, "Failed to read Chirp temperature: %s", e.what());
            }

            return reading;
        },
        1s
    };
};

inline PeripheralFactory makeFactoryForChirpSoilSensor() {
    return makePeripheralFactory<ChirpSoilSensor, ChirpSoilSensor, ChirpSoilSensorSettings>(
        "soil:chirp",
        "environment",
        [](PeripheralInitParameters& params, const std::shared_ptr<ChirpSoilSensorSettings>& settings) {
            I2CConfig i2cConfig = settings->parse(0x20);
            auto sensor = std::make_shared<ChirpSoilSensor>(
                params.name,
                params.services.i2c,
                i2cConfig,
                settings->air.get(),
                settings->water.get());
            params.registerFeature("moisture", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getMoisture();
            });
            params.registerFeature("temperature", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getTemperature();
            });
            return sensor;
        });
}

}    // namespace farmhub::peripherals::environment
