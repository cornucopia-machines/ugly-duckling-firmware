#pragma once

#include <chrono>
#include <limits>

#include <Pin.hpp>
#include <Telemetry.hpp>
#include <utility>

using cornucopia::ugly_duckling::kernel::PinPtr;

namespace cornucopia::ugly_duckling::kernel::drivers {

struct BatteryParameters {
    /**
     * @brief Maximum voltage of the battery in millivolts.
     *
     */
    const int maximumVoltage;
    /**
     * @brief Do not boot if battery is below this threshold in millivolts.
     */
    const int bootThreshold;

    /**
     * @brief Shutdown if battery drops below this threshold in millivolts.
     */
    const int shutdownThreshold;
};

class BatteryDriver {
public:
    BatteryDriver(const BatteryParameters& parameters)
        : parameters(parameters) {
    }

    virtual ~BatteryDriver() = default;

    /**
     * @brief Get the battery voltage.
     *
     * @return Battery voltage in millivolts, or -1 if the read failed.
     */
    virtual int getVoltage() = 0;

    virtual double getPercentage() {
        int voltage = getVoltage();
        if (voltage < 0) {
            return -1.0;
        }
        auto percentage = static_cast<double>(voltage - parameters.shutdownThreshold) /
               (parameters.maximumVoltage - parameters.shutdownThreshold) * 100.0;
        if (percentage < 0) {
            return 0.0;
        }
        if (percentage > 100) {
            return 100.0;
        }
        return percentage;
    }

    /**
     * @brief Get the current, if supported.
     *
     * @return Consumed current in mA, or std::nullopt if not supported.
     * @note The current is positive when discharging, negative when charging.
     */
    virtual std::optional<double> getCurrent() {
        return std::nullopt;
    }

    virtual std::optional<seconds> getTimeToEmpty() {
        return std::nullopt;
    }

    const BatteryParameters parameters;
};

class AnalogBatteryDriver
    : public BatteryDriver {
public:
    AnalogBatteryDriver(const InternalPinPtr& pin, double voltageDividerRatio, const BatteryParameters& parameters)
        : BatteryDriver(parameters)
        , analogPin(pin)
        , voltageDividerRatio(voltageDividerRatio) {
        LOGI("Initializing analog battery driver on pin %s",
            analogPin.getName().c_str());
    }

    int getVoltage() override {
        for (int trial = 0; trial < 5; trial++) {
            auto batteryLevel = analogPin.tryAnalogRead();
            if (!batteryLevel.has_value()) {
                LOGE("Failed to read battery level");
                continue;
            }
            return static_cast<int>(batteryLevel.value() * 3.3 / 4096 * voltageDividerRatio * 1000);
        }
        return -1;
    }

private:
    AnalogPin analogPin;
    const double voltageDividerRatio;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
