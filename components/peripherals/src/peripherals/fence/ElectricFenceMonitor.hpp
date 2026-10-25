#pragma once
#include <PulseCounter.hpp>
#include <Queue.hpp>
#include <Telemetry.hpp>
#include <peripherals/Peripheral.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::peripherals;

namespace cornucopia::ugly_duckling::peripherals::fence {

struct FencePinConfig {
    InternalPinPtr pin;
    uint16_t voltage {};
};

}    // namespace cornucopia::ugly_duckling::peripherals::fence

template <>
struct cornucopia::ugly_duckling::kernel::config::PropertyValidator<cornucopia::ugly_duckling::peripherals::fence::FencePinConfig> {
    static Result<> validate(const cornucopia::ugly_duckling::peripherals::fence::FencePinConfig& config, JsonVariantConst json) {
        return PropertyValidator<InternalPinPtr>::validate(config.pin, json["pin"]);
    }
};

namespace cornucopia::ugly_duckling::peripherals::fence {

class ElectricFenceMonitorSettings
    : public ConfigurationSection {
public:
    ArrayProperty<FencePinConfig> pins { this, "pins" };
    Property<seconds> measurementFrequency { this, "measurementFrequency", 10s };
};

class ElectricFenceMonitor final
    : public Peripheral {
public:
    static Result<std::shared_ptr<ElectricFenceMonitor>> create(
        const std::string& name,
        const std::shared_ptr<PulseCounterManager>& pulseCounterManager,
        const std::shared_ptr<ElectricFenceMonitorSettings>& settings) {

        std::string pinsDescription;
        for (const auto& pinConfig : settings->pins.get()) {
            if (!pinsDescription.empty()) {
                pinsDescription += ", ";
            }
            pinsDescription += pinConfig.pin->getName() + "=" + std::to_string(pinConfig.voltage) + "V";
        }
        LOGI("Initializing electric fence with pins %s", pinsDescription.c_str());

        std::vector<FencePin> pins;
        for (const auto& pinConfig : settings->pins.get()) {
            auto unit = TRY(pulseCounterManager->create({
                .pin = pinConfig.pin,
            }));
            pins.emplace_back(pinConfig.voltage, unit);
        }

        return std::shared_ptr<ElectricFenceMonitor>(new ElectricFenceMonitor(name, std::move(pins), settings->measurementFrequency.get()));
    }

    double getVoltage() const {
        return lastVoltage.load();
    }

private:
    struct FencePin {
        uint16_t voltage;
        std::shared_ptr<PulseCounter> counter;
    };

    ElectricFenceMonitor(
        const std::string& name,
        std::vector<FencePin>&& fencePins,
        milliseconds measurementFrequency)
        : Peripheral(name)
        , pins(std::move(fencePins)) {

        Task::loop(name, 3072, [this, measurementFrequency](Task& task) {
            uint16_t lastVoltage = 0;
            for (auto& pin : pins) {
                uint32_t count = pin.counter->resetCount();

                if (count > 0) {
                    lastVoltage = std::max(pin.voltage, lastVoltage);
                    LOGV("Counted %" PRIu32 " pulses on pin %s (voltage: %dV)",
                        count, pin.counter->getPin()->getName().c_str(), pin.voltage);
                }
            }
            this->lastVoltage = lastVoltage;
            LOGV("Last voltage: %d",
                lastVoltage);
            task.delayUntil(measurementFrequency);
        });
    }

    std::atomic<uint16_t> lastVoltage { 0 };

    std::vector<FencePin> pins;
};

inline PeripheralFactory makeFactory() {
    return makePeripheralFactory<ElectricFenceMonitor, ElectricFenceMonitorSettings>(
        "electric-fence",
        "electric-fence",
        [](PeripheralInitParameters& params, const std::shared_ptr<ElectricFenceMonitorSettings>& settings) -> Result<std::shared_ptr<ElectricFenceMonitor>> {
            auto monitor = TRY(ElectricFenceMonitor::create(params.name, params.services.pulseCounterManager, settings));
            params.registerFeature("voltage", [monitor](JsonObject& telemetryJson) {
                telemetryJson["value"] = monitor->getVoltage();
            });
            return monitor;
        });
}

}    // namespace cornucopia::ugly_duckling::peripherals::fence

namespace ArduinoJson {

using cornucopia::ugly_duckling::peripherals::fence::FencePinConfig;

template <>
struct Converter<FencePinConfig> {
    static bool toJson(const FencePinConfig& src, JsonVariant dst) {
        dst["pin"] = src.pin;
        dst["voltage"] = src.voltage;
        return true;
    }

    static FencePinConfig fromJson(JsonVariantConst src) {
        FencePinConfig dst;
        dst.pin = src["pin"];
        dst.voltage = src["voltage"];
        return dst;
    }

    static bool checkJson(JsonVariantConst src) {
        return src.is<JsonObjectConst>();
    }
};

}    // namespace ArduinoJson
