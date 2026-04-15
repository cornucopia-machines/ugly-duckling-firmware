#pragma once

#include <chrono>

#include <ArduinoJson.h>

#include <Concurrent.hpp>
#include <PulseCounter.hpp>
#include <Task.hpp>
#include <Telemetry.hpp>
#include <mqtt/MqttDriver.hpp>
#include <utility>

#include <peripherals/Peripheral.hpp>
#include <peripherals/api/IFlowMeter.hpp>

using namespace std::chrono;
using namespace cornucopia::ugly_duckling::kernel::mqtt;
using namespace cornucopia::ugly_duckling::peripherals::api;

namespace cornucopia::ugly_duckling::peripherals::flow_meter {

class FlowMeterSettings
    : public ConfigurationSection {
public:
    Property<InternalPinPtr> pin { this, "pin" };
    // Default Q factor for YF-S201 flow sensor
    Property<double> qFactor { this, "qFactor", 7.5 };
    Property<milliseconds> measurementFrequency { this, "measurementFrequency", 1s };
};

class FlowMeter final
    : public virtual api::IFlowMeter,
      public Peripheral {
public:
    FlowMeter(
        const std::string& name,
        const std::shared_ptr<PulseCounterManager>& pulseCounterManager,
        const InternalPinPtr& pin,
        double qFactor,
        milliseconds measurementFrequency)
        : Peripheral(name)
        , qFactor(qFactor) {

        LOGI("Initializing flow meter on pin %s with Q = %.2f",
            pin->getName().c_str(), qFactor);

        counter = pulseCounterManager->create({
            .pin = pin,
            .debounceTime = 1ms,
        });

        auto now = steady_clock::now();
        lastMeasurement = now;
        lastSeenFlow = now;
        lastPublished = now;

        Task::loop(name, 3072, [this, measurementFrequency](Task& task) {
            auto now = steady_clock::now();
            milliseconds elapsed = duration_cast<milliseconds>(now - lastMeasurement);
            if (elapsed.count() > 0) {
                lastMeasurement = now;

                uint32_t pulses = counter->reset();

                if (pulses > 0) {
                    Lock lock(updateMutex);
                    double currentVolume = pulses / this->qFactor / 60.0F;
                    LOGV("Counted %" PRIu32 " pulses, %.2f l/min, %.2f l",
                        pulses, currentVolume / (elapsed.count() / 1000.0F / 60.0F), currentVolume);
                    volume += currentVolume;
                    lastSeenFlow = now;
                }
            }
            task.delayUntil(measurementFrequency);
        });
    }

    double getVolume() override {
        Lock lock(updateMutex);
        return getVolumeAndReset();
    }

    void populateTelemetry(JsonObject& json) {
        Lock lock(updateMutex);
        populateTelemetryUnderLock(json);
    }

private:
    void populateTelemetryUnderLock(JsonObject& json) {
        getVolumeAndReset();
        auto currentVolume = unpublishedVolume;
        unpublishedVolume = 0.0;

        // Volume is measured in liters
        json["volume"] = currentVolume;
        auto duration = duration_cast<microseconds>(lastMeasurement - lastPublished);
        if (duration > microseconds::zero()) {
            // Flow rate is measured in in liters / min
            json["rate"] = currentVolume / static_cast<double>(duration.count()) * 1000 * 1000 * 60;
        }
        lastPublished = lastMeasurement;
    }

    double getVolumeAndReset() {
        double currentVolume = volume;
        volume = 0.0;
        unpublishedVolume += currentVolume;
        return currentVolume;
    }

    std::shared_ptr<PulseCounter> counter;
    const double qFactor;

    steady_clock::time_point lastMeasurement;
    steady_clock::time_point lastSeenFlow;
    steady_clock::time_point lastPublished;
    double volume = 0.0;
    double unpublishedVolume = 0.0;

    Mutex updateMutex;
};

inline PeripheralFactory makeFactory() {
    return makePeripheralFactory<FlowMeter, FlowMeterSettings, IFlowMeter>(
        "flow-meter",
        "flow-meter",
        [](PeripheralInitParameters& params, const std::shared_ptr<FlowMeterSettings>& settings) {
            auto meter = std::make_shared<FlowMeter>(
                params.name,
                params.services.pulseCounterManager,
                settings->pin.get(),
                settings->qFactor.get(),
                settings->measurementFrequency.get());
            params.registerFeature("flow", [meter](JsonObject& telemetry) {
                meter->populateTelemetry(telemetry);
            });
            return meter;
        });
}

}    // namespace cornucopia::ugly_duckling::peripherals::flow_meter
