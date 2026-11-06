#pragma once
#include <I2CManager.hpp>
#include <Telemetry.hpp>
#include <config/Configuration.hpp>
#include <peripherals/I2CSettings.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/light_sensor/LightSensor.hpp>

#include <tsl2591.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

using namespace std::chrono;
using namespace std::chrono_literals;

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::peripherals;

namespace cornucopia::ugly_duckling::peripherals::light_sensor {

static constexpr uint8_t TSL2591_ADDR = 0x29;

class Tsl2591Settings
    : public I2CSettings {
public:
    // Infrequent by default: on MK14 every reading powers up the switched rail
    Property<seconds> measurementFrequency { this, "measurementFrequency", 1min };
    Property<seconds> latencyInterval { this, "latencyInterval", 5min };
};

class Tsl2591 final
    : public LightSensor {
public:
    static Result<std::shared_ptr<Tsl2591>> create(
        const std::string& name,
        const std::shared_ptr<I2CManager>& i2c,
        const std::shared_ptr<SwitchedRail>& rail,
        const I2CConfig& config,
        seconds measurementFrequency,
        seconds latencyInterval) {
        auto bus = TRY(i2c->getBusFor(config));

        LOGI("Initializing TSL2591 light sensor with %s",
            config.toString().c_str());

        auto sensor = std::shared_ptr<Tsl2591>(new Tsl2591(name, bus, rail, measurementFrequency, latencyInterval));
        ESP_TRY(tsl2591_init_desc(&sensor->sensor, bus->port, bus->sda->getGpio(), bus->scl->getGpio()));
        TRY(sensor->railDevice.initialize());

        sensor->runLoop();
        return sensor;
    }

    ~Tsl2591() override {
        tsl2591_free_desc(&sensor);
    }

    Tsl2591(const Tsl2591&) = delete;
    Tsl2591& operator=(const Tsl2591&) = delete;
    Tsl2591(Tsl2591&&) = delete;
    Tsl2591& operator=(Tsl2591&&) = delete;

private:
    Tsl2591(
        const std::string& name,
        const std::shared_ptr<I2CBus>& bus,
        const std::shared_ptr<SwitchedRail>& rail,
        seconds measurementFrequency,
        seconds latencyInterval)
        : LightSensor(name, measurementFrequency, latencyInterval)
        , bus(bus)
        , railDevice(rail, 0us, [this]() -> Result<> {
            return configure();
        }) {
    }

    Result<> configure() {
        auto* device = &sensor;
        ESP_TRY(tsl2591_init(device));

        // TODO Make these configurable
        ESP_TRY(tsl2591_set_power_status(device, TSL2591_POWER_ON));
        ESP_TRY(tsl2591_set_als_status(device, TSL2591_ALS_ON));
        ESP_TRY(tsl2591_set_gain(device, TSL2591_GAIN_MEDIUM));
        ESP_TRY(tsl2591_set_integration_time(device, TSL2591_INTEGRATION_300MS));
        // Wait for the first integration cycle with these settings, or we'd read zero after a power-up
        Task::delay(INTEGRATION_TIME + 10ms);
        return {};
    }

    static constexpr milliseconds INTEGRATION_TIME { 300 };

protected:
    std::optional<double> readLightLevel() override {
        auto session = railDevice.power();
        if (!session) {
            LOGD("Could not set up light sensor: %s", session.error().message.c_str());
            return std::nullopt;
        }
        float lux;
        esp_err_t res = tsl2591_get_lux(&sensor, &lux);
        if (res != ESP_OK) {
            LOGD("Could not read light level: %s", esp_err_to_name(res));
            return std::nullopt;
        }
        return static_cast<double>(lux);
    }

private:
    std::shared_ptr<I2CBus> bus;
    tsl2591_t sensor {};
    RailDevice railDevice;
};

inline PeripheralFactory makeFactoryForTsl2591() {
    return makePeripheralFactory<Tsl2591, Tsl2591Settings, ILightSensor>(
        "light-sensor:tsl2591",
        "light-sensor",
        [](PeripheralInitParameters& params, const std::shared_ptr<Tsl2591Settings>& settings) -> Result<std::shared_ptr<Tsl2591>> {
            I2CConfig i2cConfig = settings->parse(TSL2591_ADDR);
            auto sensor = TRY(Tsl2591::create(params.name, params.services.i2c, params.services.switchedRail, i2cConfig, settings->measurementFrequency.get(), settings->latencyInterval.get()));
            params.registerFeature("light", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getLightLevel();
            });
            return sensor;
        });
}

}    // namespace cornucopia::ugly_duckling::peripherals::light_sensor
