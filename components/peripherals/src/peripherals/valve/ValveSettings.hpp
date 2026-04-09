#pragma once

#include <chrono>

#include <ArduinoJson.h>

#include <Configuration.hpp>

#include <peripherals/api/IValve.hpp>
#include <peripherals/valve/ValveControlStrategy.hpp>

using namespace std::chrono;
using namespace std::chrono_literals;

using namespace cornucopia::ugly_duckling::kernel;

namespace cornucopia::ugly_duckling::peripherals::valve {

class ValveSettings
    : public ConfigurationSection {
public:
    ValveSettings(ValveControlStrategyType defaultStrategy)
        : strategy(this, "strategy", defaultStrategy) {
    }

    /**
     * @brief The pin to use to control the valve.
     *
     * @details This can be an internal or an external pin. When specified, the motor is ignored.
     */
    Property<PinPtr> pin { this, "pin" };

    /**
     * @brief The name of the motor service to use to control the valve.
     *
     * @details When the pin is specified, this is ignored.
     */
    Property<std::string> motor { this, "motor" };

    /**
     * @brief The strategy to use to control the motorized valve.
     *
     * @details Ignored when the pin is specified.
     */
    Property<ValveControlStrategyType> strategy;

    /**
     * @brief Duty to use to hold the motorized valve in place.
     *
     * @details This is a percentage from 0 to 100, default is 100%. This is ignored for latching strategies and when the pin is specified.
     */
    Property<double> holdDuty { this, "holdDuty", 100 };    // This is a percentage

    /**
     * @brief Duration to keep the motor running to switch the motorized valve.
     *
     * @details This is in milliseconds, default is 500ms. This is ignored when the pin is specified.
     */
    Property<milliseconds> switchDuration { this, "switchDuration", 500ms };

    std::unique_ptr<ValveControlStrategy> createValveControlStrategy(const std::map<std::string, std::shared_ptr<PwmMotorDriver>>& motors, const std::string& motorName) const {
        PinPtr pin = this->pin.get();
        if (pin != nullptr) {
            return std::make_unique<LatchingPinValveControlStrategy>(pin);
        }

        auto motor = findMotor(motors, motorName);
        auto switchDuration = this->switchDuration.get();
        auto holdDuty = this->holdDuty.get() / 100.0;

        switch (this->strategy.get()) {
            case ValveControlStrategyType::NormallyOpen:
                return std::make_unique<NormallyOpenMotorValveControlStrategy>(motor, switchDuration, holdDuty);
            case ValveControlStrategyType::NormallyClosed:
                return std::make_unique<NormallyClosedMotorValveControlStrategy>(motor, switchDuration, holdDuty);
            case ValveControlStrategyType::Latching:
                return std::make_unique<LatchingMotorValveControlStrategy>(motor, switchDuration, holdDuty);
            default:
                throw PeripheralCreationException("unknown strategy");
        }
    }
};

}    // namespace cornucopia::ugly_duckling::peripherals::valve

namespace ArduinoJson {

using cornucopia::ugly_duckling::peripherals::valve::ValveControlStrategyType;

template <>
struct Converter<ValveControlStrategyType> {
    static bool toJson(const ValveControlStrategyType& src, JsonVariant dst) {
        switch (src) {
            case ValveControlStrategyType::NormallyOpen:
                return dst.set("NO");
            case ValveControlStrategyType::NormallyClosed:
                return dst.set("NC");
            case ValveControlStrategyType::Latching:
                return dst.set("latching");
            default:
                LOGE("Unknown strategy: %d", static_cast<int>(src));
                return dst.set("NC");
        }
    }

    static ValveControlStrategyType fromJson(JsonVariantConst src) {
        auto strategy = src.as<std::string>();
        if (strategy == "NO") {
            return ValveControlStrategyType::NormallyOpen;
        }
        if (strategy == "NC") {
            return ValveControlStrategyType::NormallyClosed;
        }
        if (strategy == "latching") {
            return ValveControlStrategyType::Latching;
        }
        LOGE("Unknown strategy: %s", strategy.c_str());
        return ValveControlStrategyType::NormallyClosed;
    }

    static bool checkJson(JsonVariantConst src) {
        return src.is<std::string>();
    }
};

}    // namespace ArduinoJson
