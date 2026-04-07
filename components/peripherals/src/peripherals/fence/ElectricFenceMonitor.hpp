#pragma once

#include <chrono>
#include <vector>

#include <Concurrent.hpp>
#include <PulseCounter.hpp>
#include <Telemetry.hpp>

#include <peripherals/Peripheral.hpp>
#include <utility>

using namespace std::chrono_literals;
using namespace farmhub::kernel;
using namespace farmhub::peripherals;

namespace farmhub::peripherals::fence {

struct FencePinConfig {
    InternalPinPtr pin;
    uint16_t voltage {};
};

class ElectricFenceMonitorSettings
    : public ConfigurationSection {
public:
    ArrayProperty<FencePinConfig> pins { this, "pins" };
    Property<seconds> measurementFrequency { this, "measurementFrequency", 10s };
};

class ElectricFenceMonitor final
    : public Peripheral {
public:
    ElectricFenceMonitor(
        const std::string& name,
        const std::shared_ptr<PulseCounterManager>& pulseCounterManager,
        const std::shared_ptr<ElectricFenceMonitorSettings>& settings)
        : Peripheral(name) {

        std::string pinsDescription;
        for (const auto& pinConfig : settings->pins.get()) {
            if (!pinsDescription.empty()) {
                pinsDescription += ", ";
            }
            pinsDescription += pinConfig.pin->getName() + "=" + std::to_string(pinConfig.voltage) + "V";
        }
        LOGI("Initializing electric fence with pins %s", pinsDescription.c_str());

        for (const auto& pinConfig : settings->pins.get()) {
            auto unit = pulseCounterManager->create({
                .pin = pinConfig.pin,
            });
            pins.emplace_back(pinConfig.voltage, unit);
        }

        auto measurementFrequency = settings->measurementFrequency.get();
        Task::loop(name, 3072, [this, measurementFrequency](Task& task) {
            uint16_t lastVoltage = 0;
            for (auto& pin : pins) {
                uint32_t count = pin.counter->reset();

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

    double getVoltage() const {
        return lastVoltage.load();
    }

private:
    std::atomic<uint16_t> lastVoltage { 0 };

    struct FencePin {
        uint16_t voltage;
        std::shared_ptr<PulseCounter> counter;
    };

    std::vector<FencePin> pins;
};

inline PeripheralFactory makeFactory() {
    return makePeripheralFactory<ElectricFenceMonitor, ElectricFenceMonitor, ElectricFenceMonitorSettings>(
        "electric-fence",
        "electric-fence",
        [](PeripheralInitParameters& params, const std::shared_ptr<ElectricFenceMonitorSettings>& settings) {
            auto monitor = std::make_shared<ElectricFenceMonitor>(
                params.name,
                params.services.pulseCounterManager,
                settings);
            params.registerFeature("voltage", [monitor](JsonObject& telemetryJson) {
                telemetryJson["value"] = monitor->getVoltage();
            });
            return monitor;
        });
}

}    // namespace farmhub::peripherals::fence

namespace ArduinoJson {

using farmhub::peripherals::fence::FencePinConfig;

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
