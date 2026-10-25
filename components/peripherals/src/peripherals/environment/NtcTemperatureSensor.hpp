#pragma once
#include <Log.hpp>
#include <Pin.hpp>
#include <config/Configuration.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/api/ITemperatureSensor.hpp>
#include <utils/DebouncedMeasurement.hpp>

#include <cmath>
#include <memory>
#include <string>

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::peripherals::api;

namespace cornucopia::ugly_duckling::peripherals::environment {

class NtcTemperatureSensor final
    : virtual public ITemperatureSensor,
      public Peripheral {
public:
    NtcTemperatureSensor(
        const std::string& name,
        std::unique_ptr<AnalogPin> pin,
        double beta)
        : Peripheral(name)
        , pin(std::move(pin))
        , beta(beta) {
        LOGTI(ENV, "Initializing NTC temperature sensor on pin '%s' with beta = %.1f",
            this->pin->getName().c_str(), beta);
    }

    Celsius getTemperature() override {
        return measurement.getValue();
    }

private:
    const std::unique_ptr<AnalogPin> pin;
    double beta;

    utils::DebouncedMeasurement<Celsius> measurement {
        [this](const utils::DebouncedParams<Celsius> _params) -> std::optional<Celsius> {
            const auto reading = pin->tryAnalogRead();
            if (!reading) {
                return std::nullopt;
            }
            const auto analogValue = *reading;
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
    return makePeripheralFactory<NtcTemperatureSensor, NtcTemperatureSensorSettings, ITemperatureSensor>(
        "environment:ntc-temperature-sensor",
        "environment",
        [](PeripheralInitParameters& params, const std::shared_ptr<NtcTemperatureSensorSettings>& settings) -> Result<std::shared_ptr<NtcTemperatureSensor>> {
            auto pin = TRY(AnalogPin::create(settings->pin.get()));
            auto sensor = std::make_shared<NtcTemperatureSensor>(
                params.name,
                std::move(pin),
                settings->beta.get());

            params.registerFeature("temperature", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getTemperature();
            });
            return sensor;
        });
}

}    // namespace cornucopia::ugly_duckling::peripherals::environment
