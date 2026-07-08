#pragma once

#include <limits>
#include <utility>

#include <sht3x.h>

#include <I2CManager.hpp>
#include <utils/DebouncedMeasurement.hpp>

#include <peripherals/I2CSettings.hpp>
#include <peripherals/Peripheral.hpp>

#include "Environment.hpp"

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
        return measurement.getValue().first;
    }

    double getMoisture() override {
        return measurement.getValue().second;
    }

private:
    using Reading = std::pair<double, double>;
    static constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

    std::shared_ptr<I2CBus> bus;
    sht3x_t sensor {};

    utils::DebouncedMeasurement<Reading> measurement {
        [this](const utils::DebouncedParams<Reading>) -> std::optional<Reading> {
            float fTemp;
            float fHumidity;
            esp_err_t res = sht3x_measure(&sensor, &fTemp, &fHumidity);
            if (res == ESP_OK) {
                LOGTV(ENV, "Measured temperature: %.2f °C, humidity: %.2f %%",
                    fTemp, fHumidity);
                return Reading { fTemp, fHumidity };
            }
            LOGTD(ENV, "Could not measure temperature: %s", esp_err_to_name(res));
            return Reading { NaN, NaN };
        },
        std::chrono::seconds(1),
        { NaN, NaN }
    };
};

inline PeripheralFactory makeFactoryForSht3x() {
    return makePeripheralFactory<Sht3xSensor, I2CSettings>(
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
