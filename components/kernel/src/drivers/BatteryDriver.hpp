#pragma once

#include <Pin.hpp>
#include <Telemetry.hpp>

#include <chrono>
#include <limits>
#include <optional>
#include <utility>

using cornucopia::ugly_duckling::kernel::PinPtr;

namespace cornucopia::ugly_duckling::kernel::drivers {

LOGGING_TAG(BATTERY, "battery")

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
        auto percentage = static_cast<double>(voltage - parameters.shutdownThreshold) / (parameters.maximumVoltage - parameters.shutdownThreshold) * 100.0;
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
     * @return Battery current in mA, or std::nullopt if not supported.
     * @note We follow the BQ27220's convention: the current is positive when charging,
     * negative when discharging.
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
        LOGTI(BATTERY, "Initializing analog battery driver on pin %s",
            analogPin.getName().c_str());
    }

    int getVoltage() override {
        for (int trial = 0; trial < 5; trial++) {
            auto mv = analogPin.tryAnalogReadMillivolts();
            if (!mv.has_value()) {
                LOGTE(BATTERY, "Failed to read battery level");
                continue;
            }
            return static_cast<int>(*mv * voltageDividerRatio);
        }
        return -1;
    }

private:
    AnalogPin analogPin;
    const double voltageDividerRatio;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
