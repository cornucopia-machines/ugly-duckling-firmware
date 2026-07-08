#pragma once

#include <limits>
#include <utility>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <I2CManager.hpp>
#include <utils/DebouncedMeasurement.hpp>

#include <peripherals/I2CSettings.hpp>
#include <peripherals/Peripheral.hpp>

#include "Environment.hpp"

using namespace cornucopia::ugly_duckling::peripherals;

namespace cornucopia::ugly_duckling::peripherals::environment {

class Hdc2010Sensor final
    : public EnvironmentSensor,
      public Peripheral {
public:
    Hdc2010Sensor(
        const std::string& name,
        const std::shared_ptr<I2CManager>& i2c,
        const I2CConfig& config)
        : Peripheral(name)
        , device(i2c->createDevice(name, config)) {

        // TODO Add a separate task to do measurements to unblock telemetry collection?

        LOGTI(ENV, "Initializing HDC2010 environment sensor '%s' with %s",
            name.c_str(), config.toString().c_str());

        // On-demand mode (ODR=000), heater off, DRDY output disabled
        device->writeRegByte(0x0E, 0x00);
    }

    double getTemperature() override {
        return measurement.getValue().first;
    }

    double getMoisture() override {
        return measurement.getValue().second;
    }

private:
    using Reading = std::pair<double, double>;
    static constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

    const std::shared_ptr<I2CDevice> device;

    utils::DebouncedMeasurement<Reading> measurement {
        [this](const utils::DebouncedParams<Reading>) -> std::optional<Reading> {
            try {
                // Trigger a single measurement (14-bit temp + humidity); meas_trig self-clears on completion
                device->writeRegByte(0x0F, 0x01);
                // 14-bit conversion takes ~1.3 ms; wait with margin
                Task::delay(3ms);
                // Burst-read TEMP_LOW (0x00), TEMP_HIGH (0x01), HUM_LOW (0x02), HUM_HIGH (0x03)
                uint8_t buf[4];
                device->readReg(0x00, buf, 4);
                uint16_t rawTemp = static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8);
                uint16_t rawHum  = static_cast<uint16_t>(buf[2]) | (static_cast<uint16_t>(buf[3]) << 8);
                // Datasheet §8.3.4: maps 0–65535 to -40…+125 °C (range = 165 °C)
                double temp = (static_cast<double>(rawTemp) / 65536.0 * 165.0) - 40.0;
                // Datasheet §8.3.4: maps 0–65535 to 0…100 %RH
                double hum  = static_cast<double>(rawHum)  / 65536.0 * 100.0;
                LOGTV(ENV, "Measured temperature: %.2f °C, humidity: %.2f %%", temp, hum);
                return Reading { temp, hum };
            } catch (const std::exception& e) {
                LOGTD(ENV, "Could not measure temperature: %s", e.what());
                return Reading { NaN, NaN };
            }
        },
        1s,
        { NaN, NaN }
    };
};

inline PeripheralFactory makeFactoryForHdc2010() {
    return makePeripheralFactory<Hdc2010Sensor, I2CSettings>(
        "environment:hdc2010",
        "environment",
        [](PeripheralInitParameters& params, const std::shared_ptr<I2CSettings>& settings) {
            I2CConfig i2cConfig = settings->parse(0x40 /* Also supports 0x41 */);
            auto sensor = std::make_shared<Hdc2010Sensor>(
                params.name,
                params.services.i2c,
                i2cConfig);
            params.registerFeature("temperature", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getTemperature();
            });
            params.registerFeature("moisture", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getMoisture();
            });
            return sensor;
        });
}

}    // namespace cornucopia::ugly_duckling::peripherals::environment
