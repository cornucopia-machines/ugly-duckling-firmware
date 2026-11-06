#pragma once
#include <I2CManager.hpp>
#include <Telemetry.hpp>
#include <config/Configuration.hpp>
#include <peripherals/I2CSettings.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/light_sensor/LightSensor.hpp>

#include <bh1750.h>
#include <esp_system.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

using namespace std::chrono;
using namespace std::chrono_literals;

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::peripherals;

namespace cornucopia::ugly_duckling::peripherals::light_sensor {

class Bh1750Settings
    : public I2CSettings {
public:
    // Infrequent by default: on MK14 every reading powers up the switched rail
    Property<seconds> measurementFrequency { this, "measurementFrequency", 1min };
    Property<seconds> latencyInterval { this, "latencyInterval", 5min };
};

class Bh1750 final
    : public LightSensor {
public:
    static Result<std::shared_ptr<Bh1750>> create(
        const std::string& name,
        const std::shared_ptr<SwitchedRail>& rail,
        const I2CConfig& config,
        seconds measurementFrequency,
        seconds latencyInterval) {
        LOGI("Initializing BH1750 light sensor with %s",
            config.toString().c_str());

        auto sensor = std::shared_ptr<Bh1750>(new Bh1750(name, rail, measurementFrequency, latencyInterval));
        // TODO Use I2CManager to create device
        ESP_TRY(bh1750_init_desc(&sensor->railDevice.unpowered(), config.address, I2C_NUM_0, config.sda->getGpio(), config.scl->getGpio()));
        TRY(sensor->railDevice.initialize());

        sensor->runLoop();
        return sensor;
    }

    ~Bh1750() override {
        bh1750_free_desc(&railDevice.unpowered());
    }

    Bh1750(const Bh1750&) = delete;
    Bh1750& operator=(const Bh1750&) = delete;
    Bh1750(Bh1750&&) = delete;
    Bh1750& operator=(Bh1750&&) = delete;

protected:
    std::optional<double> readLightLevel() override {
        auto device = railDevice.power();
        if (!device) {
            LOGE("Could not set up light sensor: %s", device.error().message.c_str());
            return std::nullopt;
        }
        uint16_t lightLevel;
        if (bh1750_read(&**device, &lightLevel) != ESP_OK) {
            LOGE("Could not read light level");
            return std::nullopt;
        }
        return lightLevel;
    }

private:
    Bh1750(
        const std::string& name,
        const std::shared_ptr<SwitchedRail>& rail,
        seconds measurementFrequency,
        seconds latencyInterval)
        : LightSensor(name, measurementFrequency, latencyInterval)
        , railDevice(rail, 0us, [](i2c_dev_t& sensor) -> Result<> {
            ESP_TRY(bh1750_setup(&sensor, BH1750_MODE_CONTINUOUS, BH1750_RES_LOW));
            // Wait for the first measurement after a power-up, or we'd read zero
            Task::delay(FIRST_MEASUREMENT_TIME);
            return {};
        }) {
    }

    // Maximum L-resolution measurement time
    static constexpr milliseconds FIRST_MEASUREMENT_TIME { 24 };

    RailDevice<i2c_dev_t> railDevice;
};

inline PeripheralFactory makeFactoryForBh1750() {
    return makePeripheralFactory<Bh1750, Bh1750Settings, ILightSensor>(
        "light-sensor:bh1750",
        "light-sensor",
        [](PeripheralInitParameters& params, const std::shared_ptr<Bh1750Settings>& settings) -> Result<std::shared_ptr<Bh1750>> {
            I2CConfig i2cConfig = settings->parse(0x23);
            auto sensor = TRY(Bh1750::create(params.name, params.services.switchedRail, i2cConfig, settings->measurementFrequency.get(), settings->latencyInterval.get()));
            params.registerFeature("light", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getLightLevel();
            });
            return sensor;
        });
}

}    // namespace cornucopia::ugly_duckling::peripherals::light_sensor
