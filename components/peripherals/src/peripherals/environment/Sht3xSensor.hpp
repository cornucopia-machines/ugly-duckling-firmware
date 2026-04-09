#pragma once

#include <chrono>
#include <limits>
#include <utility>

#include <sht3x.h>

#include <Concurrent.hpp>
#include <I2CManager.hpp>

#include <peripherals/I2CSettings.hpp>
#include <peripherals/Peripheral.hpp>

#include "Environment.hpp"

using namespace std::chrono;
using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::peripherals;

namespace cornucopia::ugly_duckling::peripherals::environment {

class Sht3xSensor final
    : public EnvironmentSensor,
      public Peripheral {
public:
    Sht3xSensor(
        const std::string& name,
        const std::string& sensorType,
        const std::shared_ptr<I2CManager>& i2c,
        const I2CConfig& config)
        : Peripheral(name)
        , bus(i2c->getBusFor(config)) {

        // TODO Add commands to soft/hard reset the sensor
        // TODO Add configuration for fast / slow measurement
        // TODO Add a separate task to do measurements to unblock telemetry collection?

        LOGTI(ENV, "Initializing %s environment sensor '%s' with %s",
            sensorType.c_str(), name.c_str(), config.toString().c_str());

        ESP_ERROR_THROW(sht3x_init_desc(&sensor, config.address, bus->port, bus->sda->getGpio(), bus->scl->getGpio()));
        ESP_ERROR_THROW(sht3x_init(&sensor));
    }

    double getTemperature() override {
        Lock lock(mutex);
        updateMeasurement();
        return temperature;
    }

    double getMoisture() override {
        Lock lock(mutex);
        updateMeasurement();
        return humidity;
    }

private:
    void updateMeasurement() {
        auto now = steady_clock::now();
        if (now - this->lastMeasurementTime < 1s) {
            // Do not measure more often than once per second
            return;
        }
        float fTemp;
        float fHumidity;
        esp_err_t res = sht3x_measure(&sensor, &fTemp, &fHumidity);
        if (res == ESP_OK) {
            LOGTV(ENV, "Measured temperature: %.2f °C, humidity: %.2f %%",
                fTemp, fHumidity);
            temperature = fTemp;
            humidity = fHumidity;
        } else {
            LOGTD(ENV, "Could not measure temperature: %s", esp_err_to_name(res));
            temperature = std::numeric_limits<double>::quiet_NaN();
            humidity = std::numeric_limits<double>::quiet_NaN();
        }
        this->lastMeasurementTime = now;
    }

    std::shared_ptr<I2CBus> bus;
    sht3x_t sensor {};

    Mutex mutex;
    std::chrono::steady_clock::time_point lastMeasurementTime;
    double temperature = std::numeric_limits<double>::quiet_NaN();
    double humidity = std::numeric_limits<double>::quiet_NaN();
};

inline PeripheralFactory makeFactoryForSht3x() {
    return makePeripheralFactory<Sht3xSensor, Sht3xSensor, I2CSettings>(
        "environment:sht3x",
        "environment",
        [](PeripheralInitParameters& params, const std::shared_ptr<I2CSettings>& settings) {
            I2CConfig i2cConfig = settings->parse(0x44 /* Also supports 0x45 */);
            auto sensor = std::make_shared<Sht3xSensor>(
                params.name,
                "sht3x",
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
