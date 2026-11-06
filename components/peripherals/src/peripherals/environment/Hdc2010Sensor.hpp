#pragma once
#include "Environment.hpp"
#include <I2CManager.hpp>
#include <peripherals/I2CSettings.hpp>
#include <peripherals/Peripheral.hpp>
#include <utils/DebouncedMeasurement.hpp>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <limits>
#include <memory>
#include <string>
#include <utility>

using namespace cornucopia::ugly_duckling::peripherals;

namespace cornucopia::ugly_duckling::peripherals::environment {

class Hdc2010Sensor final
    : public EnvironmentSensor,
      public Peripheral {
public:
    static Result<std::shared_ptr<Hdc2010Sensor>> create(
        const std::string& name,
        const std::shared_ptr<I2CManager>& i2c,
        const std::shared_ptr<SwitchedRail>& rail,
        const I2CConfig& config) {
        auto device = TRY(i2c->createDevice(name, config));

        // TODO Add a separate task to do measurements to unblock telemetry collection?

        LOGTI(ENV, "Initializing HDC2010 environment sensor '%s' with %s",
            name.c_str(), config.toString().c_str());

        auto sensor = std::shared_ptr<Hdc2010Sensor>(new Hdc2010Sensor(name, device, rail));
        TRY(sensor->switchedDevice.initialize());
        return sensor;
    }

    double getTemperature() override {
        return measurement.getValue().first;
    }

    double getMoisture() override {
        return measurement.getValue().second;
    }

private:
    Hdc2010Sensor(const std::string& name, const std::shared_ptr<I2CDevice>& i2cDevice, const std::shared_ptr<SwitchedRail>& rail)
        : Peripheral(name)
        , switchedDevice(rail, STARTUP_TIME, [](std::shared_ptr<I2CDevice>& device) -> Result<> {
            // On-demand mode (ODR=000), heater off, DRDY output disabled
            return device->writeRegByte(0x0E, 0x00); }, i2cDevice) {
    }

    // Datasheet §7.5.2: ready at most 3 ms after power-up
    static constexpr std::chrono::milliseconds STARTUP_TIME { 3 };

    using Reading = std::pair<double, double>;
    static constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

    SwitchedDevice<std::shared_ptr<I2CDevice>> switchedDevice;

    utils::DebouncedMeasurement<Reading> measurement {
        [this](const utils::DebouncedParams<Reading>) -> std::optional<Reading> {
            auto reading = measure();
            if (!reading) {
                LOGTD(ENV, "Could not measure temperature: %s", reading.error().message.c_str());
                return Reading { NaN, NaN };
            }
            return *reading;
        },
        1s,
        { NaN, NaN }
    };

    Result<Reading> measure() {
        auto device = TRY(switchedDevice.power());
        // Trigger a single measurement (14-bit temp + humidity); meas_trig self-clears on completion
        TRY(device->writeRegByte(0x0F, 0x01));
        // 14-bit conversion takes ~1.3 ms; wait with margin
        Task::delay(3ms);
        // Burst-read TEMP_LOW (0x00), TEMP_HIGH (0x01), HUM_LOW (0x02), HUM_HIGH (0x03)
        uint8_t buf[4];
        TRY(device->readReg(0x00, buf, 4));
        auto rawTemp = static_cast<uint16_t>(buf[0] | (buf[1] << 8));
        auto rawHum = static_cast<uint16_t>(buf[2] | (buf[3] << 8));
        // Datasheet §8.3.4: maps 0–65535 to -40…+125 °C (range = 165 °C)
        double temp = (static_cast<double>(rawTemp) / 65536.0 * 165.0) - 40.0;
        // Datasheet §8.3.4: maps 0–65535 to 0…100 %RH
        double hum = static_cast<double>(rawHum) / 65536.0 * 100.0;
        LOGTV(ENV, "Measured temperature: %.2f °C, humidity: %.2f %%", temp, hum);
        return Reading { temp, hum };
    }
};

inline PeripheralFactory makeFactoryForHdc2010() {
    return makePeripheralFactory<Hdc2010Sensor, I2CSettings>(
        "environment:hdc2010",
        "environment",
        [](PeripheralInitParameters& params, const std::shared_ptr<I2CSettings>& settings) -> Result<std::shared_ptr<Hdc2010Sensor>> {
            I2CConfig i2cConfig = settings->parse(0x40 /* Also supports 0x41 */);
            auto sensor = TRY(Hdc2010Sensor::create(params.name, params.services.i2c, params.services.switchedRail, i2cConfig));
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
