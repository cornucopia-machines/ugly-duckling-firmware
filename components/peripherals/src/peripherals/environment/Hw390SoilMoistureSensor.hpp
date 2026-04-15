#pragma once

#include <memory>
#include <utility>

#include <peripherals/Peripheral.hpp>
#include <peripherals/api/ISoilMoistureSensor.hpp>

#include <utils/DebouncedMeasurement.hpp>

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::peripherals;

namespace cornucopia::ugly_duckling::peripherals::environment {

class Hw390SoilMoistureSensorSettings
    : public ConfigurationSection {
public:
    Property<InternalPinPtr> pin { this, "pin" };
    Property<PinPtr> disablePin { this, "disablePin" };
    Property<milliseconds> enableDelay { this, "enableDelay", 100ms };

    // These values need calibrating for each sensor
    Property<uint16_t> air { this, "air", 3000 };
    Property<uint16_t> water { this, "water", 1000 };

    // Exponential moving average alpha (0..1); 1 = no smoothing, 0 = no updates
    Property<double> alpha { this, "alpha", 1.0 };
};

class Hw390SoilMoistureSensor final
    : public api::ISoilMoistureSensor,
      public Peripheral {
public:
    Hw390SoilMoistureSensor(
        const std::string& name,
        int airValue,
        int waterValue,
        double alpha,
        const InternalPinPtr& pin,
        const PinPtr& disablePin,
        milliseconds enableDelay = 100ms)
        : Peripheral(name)
        , airValue(airValue)
        , waterValue(waterValue)
        , alpha(alpha)
        , pin(pin)
        , disablePin(disablePin)
        , enableDelay(enableDelay) {

        LOGTI(ENV, "Initializing soil moisture sensor '%s' on pin %s; air value: %d; water value: %d; EMA alpha: %.2f; disable pin: %s; enable delay: %dms",
            name.c_str(), pin->getName().c_str(), airValue, waterValue, alpha, disablePin ? disablePin->getName().c_str() : "none", enableDelay.count());

        if (disablePin) {
            disablePin->pinMode(Pin::Mode::Output);
            disablePin->digitalWrite(1);
        }
    }

    Percent getMoisture() override {
        return measurement.getValue();
    }

private:
    const int airValue;
    const int waterValue;
    const double alpha;
    const AnalogPin pin;
    const PinPtr disablePin;
    const milliseconds enableDelay;

    std::optional<Percent> measure(const utils::DebouncedParams<Percent> params) {
        std::optional<uint16_t> soilMoistureValue = pin.tryAnalogRead();
        if (!soilMoistureValue.has_value()) {
            LOGTW(ENV, "Failed to read soil moisture value from pin %s",
                pin.getName().c_str());
            return std::nullopt;
        }
        LOGTV(ENV, "Soil moisture value: %d",
            soilMoistureValue.value());

        const double run = waterValue - airValue;
        const double rise = 100;
        const double delta = soilMoistureValue.value() - airValue;
        double currentValue = (delta * rise) / run;

        if (std::isnan(params.lastValue)) {
            return currentValue;
        }
        return (alpha * currentValue) + ((1 - alpha) * params.lastValue);
    }

    utils::DebouncedMeasurement<Percent> measurement {
        [this](const utils::DebouncedParams<Percent> params) -> std::optional<Percent> {
            if (disablePin) {
                disablePin->digitalWrite(0);
                Task::delay(enableDelay);
            }
            auto measurement = measure(params);
            if (disablePin) {
                disablePin->digitalWrite(1);
            }
            return measurement;
        },
        1s,
        NAN
    };
};

inline PeripheralFactory makeFactoryForHw390SoilMoisture(const std::string& factoryType = "soil:hw390") {
    return makePeripheralFactory<Hw390SoilMoistureSensor, Hw390SoilMoistureSensorSettings, ISoilMoistureSensor>(
        factoryType,
        "environment",
        [](PeripheralInitParameters& params, const std::shared_ptr<Hw390SoilMoistureSensorSettings>& settings) {
            auto sensor = std::make_shared<Hw390SoilMoistureSensor>(
                params.name,
                settings->air.get(),
                settings->water.get(),
                settings->alpha.get(),
                settings->pin.get(),
                settings->disablePin.get(),
                settings->enableDelay.get());
            params.registerFeature("moisture", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getMoisture();
            });
            return sensor;
        });
}

}    // namespace cornucopia::ugly_duckling::peripherals::environment
