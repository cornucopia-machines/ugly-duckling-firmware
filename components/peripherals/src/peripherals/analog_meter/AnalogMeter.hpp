#pragma once
#include <Log.hpp>
#include <MovingAverage.hpp>
#include <Named.hpp>
#include <Pin.hpp>
#include <Queue.hpp>
#include <Task.hpp>
#include <config/Configuration.hpp>
#include <mqtt/MqttDriver.hpp>
#include <peripherals/Peripheral.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

using namespace std::chrono;
using namespace std::chrono_literals;
using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::mqtt;

namespace cornucopia::ugly_duckling::peripherals::analog_meter {

LOGGING_TAG(ANALOG_METER, "analog-meter")

class AnalogMeter final
    : Peripheral {
public:
    AnalogMeter(
        const std::string& name,
        std::unique_ptr<AnalogPin> pin,
        double offset,
        double multiplier,
        milliseconds measurementFrequency,
        std::size_t windowSize)
        : Peripheral(name)
        , pin(std::move(pin))
        , value(windowSize) {

        LOGTI(ANALOG_METER, "Initializing analog meter on pin %s",
            this->pin->getName().c_str());

        Task::loop(name, 3072, [this, measurementFrequency, offset, multiplier](Task& task) {
            auto measurement = this->pin->tryAnalogRead();
            if (measurement.has_value()) {
                auto rawValue = *measurement;
                double value = offset + (rawValue * multiplier);
                LOGTV(ANALOG_METER, "Analog value on '%s' measured at %.2f (raw: %d)",
                    this->name.c_str(), value, rawValue);
                this->value.record(value);
            }
            task.delayUntil(measurementFrequency);
        });
    }

    double getValue() {
        return value.getAverage();
    }

private:
    const std::unique_ptr<AnalogPin> pin;
    MovingAverage<double> value;
};

class AnalogMeterSettings
    : public ConfigurationSection {
public:
    Property<std::string> type { this, "type", "analog-meter" };
    Property<InternalPinPtr> pin { this, "pin" };
    Property<double> offset { this, "offset", 0.0 };
    Property<double> multiplier { this, "multiplier", 1.0 };
    Property<milliseconds> measurementFrequency { this, "measurementFrequency", 1s };
    Property<std::size_t> windowSize { this, "windowSize", 1 };
};

inline PeripheralFactory makeFactory() {
    return makePeripheralFactory<AnalogMeter, AnalogMeterSettings>(
        "analog-meter",
        "analog-meter",
        [](PeripheralInitParameters& params, const std::shared_ptr<AnalogMeterSettings>& settings) -> Result<std::shared_ptr<AnalogMeter>> {
            auto pin = TRY(AnalogPin::create(settings->pin.get()));
            auto meter = std::make_shared<AnalogMeter>(
                params.name,
                std::move(pin),
                settings->offset.get(),
                settings->multiplier.get(),
                settings->measurementFrequency.get(),
                settings->windowSize.get());

            params.registerFeature(settings->type.get(), [meter](JsonObject& telemetryJson) {
                telemetryJson["value"] = meter->getValue();
            });

            return meter;
        });
}

}    // namespace cornucopia::ugly_duckling::peripherals::analog_meter
