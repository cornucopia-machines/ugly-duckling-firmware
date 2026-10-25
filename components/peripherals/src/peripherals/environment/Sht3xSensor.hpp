#pragma once
#include "Environment.hpp"
#include <I2CManager.hpp>
#include <peripherals/I2CSettings.hpp>
#include <peripherals/Peripheral.hpp>
#include <utils/DebouncedMeasurement.hpp>

#include <sht3x.h>

#include <limits>
#include <memory>
#include <string>
#include <utility>

using namespace cornucopia::ugly_duckling::peripherals;

namespace cornucopia::ugly_duckling::peripherals::environment {

class Sht3xSensor final
    : public EnvironmentSensor,
      public Peripheral {
public:
    static Result<std::shared_ptr<Sht3xSensor>> create(
        const std::string& name,
        const std::string& sensorType,
        const std::shared_ptr<I2CManager>& i2c,
        const I2CConfig& config) {
        auto bus = TRY(i2c->getBusFor(config));

        // TODO Add commands to soft/hard reset the sensor
        // TODO Add configuration for fast / slow measurement
        // TODO Add a separate task to do measurements to unblock telemetry collection?

        LOGTI(ENV, "Initializing %s environment sensor '%s' with %s",
            sensorType.c_str(), name.c_str(), config.toString().c_str());

        auto sensor = std::shared_ptr<Sht3xSensor>(new Sht3xSensor(name, bus));
        ESP_TRY(sht3x_init_desc(&sensor->sensor, config.address, bus->port, bus->sda->getGpio(), bus->scl->getGpio()));
        ESP_TRY(sht3x_init(&sensor->sensor));
        return sensor;
    }

    ~Sht3xSensor() override {
        sht3x_free_desc(&sensor);
    }

    Sht3xSensor(const Sht3xSensor&) = delete;
    Sht3xSensor& operator=(const Sht3xSensor&) = delete;
    Sht3xSensor(Sht3xSensor&&) = delete;
    Sht3xSensor& operator=(Sht3xSensor&&) = delete;

    double getTemperature() override {
        return measurement.getValue().first;
    }

    double getMoisture() override {
        return measurement.getValue().second;
    }

private:
    Sht3xSensor(const std::string& name, const std::shared_ptr<I2CBus>& bus)
        : Peripheral(name)
        , bus(bus) {
    }

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
                    static_cast<double>(fTemp), static_cast<double>(fHumidity));
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
        [](PeripheralInitParameters& params, const std::shared_ptr<I2CSettings>& settings) -> Result<std::shared_ptr<Sht3xSensor>> {
            I2CConfig i2cConfig = settings->parse(0x44 /* Also supports 0x45 */);
            auto sensor = TRY(Sht3xSensor::create(params.name, "sht3x", params.services.i2c, i2cConfig));
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
