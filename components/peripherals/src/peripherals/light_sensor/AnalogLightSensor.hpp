#pragma once

#include <chrono>
#include <memory>

#include <esp_system.h>

#include <Configuration.hpp>
#include <Pin.hpp>
#include <Telemetry.hpp>

#include <peripherals/I2CSettings.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/light_sensor/LightSensor.hpp>
#include <utility>

using namespace std::chrono;
using namespace std::chrono_literals;

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::peripherals;

namespace cornucopia::ugly_duckling::peripherals::light_sensor {

class AnalogLightSensorSettings
    : public I2CSettings {
public:
    Property<InternalPinPtr> pin { this, "pin" };
    Property<double> gamma { this, "gamma", 0.7 };
    Property<double> rl10 { this, "rl10", 50.0 };
    Property<seconds> measurementFrequency { this, "measurementFrequency", 1s };
    Property<seconds> latencyInterval { this, "latencyInterval", 5s };
};

class AnalogLightSensor final
    : public LightSensor {
public:
    AnalogLightSensor(
        const std::string& name,
        const InternalPinPtr& pin,
        double gamma,
        double rl10,
        seconds measurementFrequency,
        seconds latencyInterval)
        : LightSensor(name, measurementFrequency, latencyInterval)
        , pin(pin)
        , gamma(gamma)
        , rl10(rl10) {

        LOGI("Initializing analog light sensor on pin %s",
            pin->getName().c_str());

        runLoop();
    }

protected:
    double readLightLevel() override {
        // Convert the analog value into lux value:
        auto analogValue = pin.analogReadAsDouble();
        auto voltage = analogValue * REFERENCE_VOLTAGE;
        auto resistance = 2000 * voltage / (1 - voltage / REFERENCE_VOLTAGE);
        auto lux = pow(rl10 * 1e3 * pow(10, gamma) / resistance, (1 / gamma));

        return lux;
    }

private:
    AnalogPin pin;
    const double gamma;
    const double rl10;

    static constexpr double REFERENCE_VOLTAGE = 5.0;
};

inline PeripheralFactory makeFactoryForAnalogLightSensor() {
    return makePeripheralFactory<ILightSensor, AnalogLightSensor, AnalogLightSensorSettings>(
        "light-sensor:analog",
        "light-sensor",
        [](PeripheralInitParameters& params, const std::shared_ptr<AnalogLightSensorSettings>& settings) {
            auto sensor = std::make_shared<AnalogLightSensor>(
                params.name,
                settings->pin.get(),
                settings->gamma.get(),
                settings->rl10.get(),
                settings->measurementFrequency.get(),
                settings->latencyInterval.get());
            params.registerFeature("light", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getLightLevel();
            });
            return sensor;
        });
}

}    // namespace cornucopia::ugly_duckling::peripherals::light_sensor
