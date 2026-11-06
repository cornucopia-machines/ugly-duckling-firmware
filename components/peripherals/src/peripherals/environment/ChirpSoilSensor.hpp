#pragma once
#include "Environment.hpp"
#include <I2CManager.hpp>
#include <peripherals/I2CSettings.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/api/ISoilMoistureSensor.hpp>
#include <peripherals/api/ITemperatureSensor.hpp>
#include <utils/DebouncedMeasurement.hpp>

#include <limits>
#include <memory>
#include <string>

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::peripherals;

namespace cornucopia::ugly_duckling::peripherals::environment {

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
    static Result<std::shared_ptr<ChirpSoilSensor>> create(
        const std::string& name,
        const std::shared_ptr<I2CManager>& i2c,
        const std::shared_ptr<SwitchedRail>& rail,
        const I2CConfig& config,
        int airValue,
        int waterValue) {
        auto device = TRY(i2c->createDevice(name, config));

        LOGTI(ENV, "Initializing Chirp soil sensor '%s' with %s; air value: %d; water value: %d",
            name.c_str(), config.toString().c_str(), airValue, waterValue);
        return std::shared_ptr<ChirpSoilSensor>(new ChirpSoilSensor(name, device, rail, airValue, waterValue));
    }

    Percent getMoisture() override {
        return measurement.getValue().moisture;
    }

    Celsius getTemperature() override {
        return measurement.getValue().temperature;
    }

private:
    ChirpSoilSensor(
        const std::string& name,
        const std::shared_ptr<I2CDevice>& i2cDevice,
        const std::shared_ptr<SwitchedRail>& rail,
        int airValue,
        int waterValue)
        : Peripheral(name)
        // Keeps no settings we change, so there is nothing to set up after a power-up
        , switchedDevice(rail, STARTUP_TIME, NO_POWER_ON_SETUP, i2cDevice)
        , airValue(airValue)
        , waterValue(waterValue) {
    }

    static constexpr uint8_t REG_CAPACITANCE = 0x00;
    static constexpr uint8_t REG_TEMPERATURE = 0x05;

    // The reference driver waits this long for the sensor to boot after a reset
    static constexpr std::chrono::seconds STARTUP_TIME { 1 };

    SwitchedDevice<std::shared_ptr<I2CDevice>> switchedDevice;
    const int airValue;
    const int waterValue;

    utils::DebouncedMeasurement<ChirpReading> measurement {
        [this](const utils::DebouncedParams<ChirpReading>) -> std::optional<ChirpReading> {
            ChirpReading reading;

            auto device = switchedDevice.power();
            if (!device) {
                LOGTW(ENV, "Failed to power Chirp sensor: %s", device.error().message.c_str());
                return reading;
            }

            // Capacitance: 16-bit big-endian
            if (auto capacitance = (*device)->readRegWord(REG_CAPACITANCE)) {
                uint16_t rawMoisture = __builtin_bswap16(*capacitance);

                double run = waterValue - airValue;
                reading.moisture = ((rawMoisture - airValue) * 100.0) / run;

                LOGTV(ENV, "Chirp capacitance: %d, moisture: %.1f%%",
                    rawMoisture, reading.moisture);
            } else {
                LOGTW(ENV, "Failed to read Chirp capacitance: %s", capacitance.error().message.c_str());
            }

            // Temperature: 16-bit big-endian, units of 1/10 °C
            if (auto temperature = (*device)->readRegWord(REG_TEMPERATURE)) {
                uint16_t rawTemp = __builtin_bswap16(*temperature);
                reading.temperature = rawTemp / 10.0;

                LOGTV(ENV, "Chirp temperature: %.1f°C", reading.temperature);
            } else {
                LOGTW(ENV, "Failed to read Chirp temperature: %s", temperature.error().message.c_str());
            }

            return reading;
        },
        1s
    };
};

inline PeripheralFactory makeFactoryForChirpSoilSensor() {
    return makePeripheralFactory<ChirpSoilSensor, ChirpSoilSensorSettings, api::ISoilMoistureSensor, api::ITemperatureSensor>(
        "soil:chirp",
        "environment",
        [](PeripheralInitParameters& params, const std::shared_ptr<ChirpSoilSensorSettings>& settings) -> Result<std::shared_ptr<ChirpSoilSensor>> {
            I2CConfig i2cConfig = settings->parse(0x20);
            auto sensor = TRY(ChirpSoilSensor::create(params.name, params.services.i2c, params.services.switchedRail, i2cConfig, settings->air.get(), settings->water.get()));
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
