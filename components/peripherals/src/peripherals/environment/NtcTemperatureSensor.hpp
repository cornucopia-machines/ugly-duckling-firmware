#pragma once

#include <cmath>

#include <Configuration.hpp>
#include <Log.hpp>
#include <Pin.hpp>

#include <peripherals/Peripheral.hpp>
#include <peripherals/api/ITemperatureSensor.hpp>

#include <utils/DebouncedMeasurement.hpp>

using namespace farmhub::kernel;
using namespace farmhub::peripherals::api;

namespace farmhub::peripherals::environment {

class NtcTemperatureSensor final
    : virtual public ITemperatureSensor,
      public Peripheral {
public:
    NtcTemperatureSensor(
        const std::string& name,
        const InternalPinPtr& pin,
        double beta)
        : Peripheral(name)
        , pin(pin)
        , beta(beta) {
        LOGTI(ENV, "Initializing NTC temperature sensor on pin '%s' with beta = %.1f",
            pin->getName().c_str(), beta);
    }

    Celsius getTemperature() override {
        return measurement.getValue();
    }

private:
    AnalogPin pin;
    double beta;

    utils::DebouncedMeasurement<Celsius> measurement {
        [this](const utils::DebouncedParams<Celsius> _params) {
            const auto analogValue = pin.analogRead();
            double celsius = (1.0 / (std::log(1.0 / (4095.0 / analogValue - 1.0)) / beta + 1.0 / 298.15)) - 273.15;
            LOGTV(ENV, "NTC temperature sensor '%s' reading: %.2f °C (raw: %d)",
                getName().c_str(), celsius, analogValue);
            return celsius;
        },
        1s,
        NAN
    };
};

struct NtcTemperatureSensorSettings
    : ConfigurationSection {
    Property<InternalPinPtr> pin { this, "pin" };
    // The beta coefficient of the thermistor
    Property<double> beta { this, "beta", 3950 };
};

inline PeripheralFactory makeFactoryForNtcTemperatureSensor() {
    return makePeripheralFactory<ITemperatureSensor, NtcTemperatureSensor, NtcTemperatureSensorSettings>(
        "environment:ntc-temperature-sensor",
        "environment",
        [](PeripheralInitParameters& params, const std::shared_ptr<NtcTemperatureSensorSettings>& settings) {
            auto sensor = std::make_shared<NtcTemperatureSensor>(
                params.name,
                settings->pin.get(),
                settings->beta.get());

            params.registerFeature("temperature", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getTemperature();
            });
            return sensor;
        });
}

}    // namespace farmhub::peripherals::environment
