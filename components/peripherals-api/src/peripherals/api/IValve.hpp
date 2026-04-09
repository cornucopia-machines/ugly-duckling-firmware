#pragma once

#include <ArduinoJson.h>

#include "IPeripheral.hpp"
#include "TargetState.hpp"

namespace cornucopia::ugly_duckling::peripherals::api {

enum class ValveState : int8_t {
    Closed = -1,
    Open = 1
};

inline static const char* toString(ValveState state) {
    switch (state) {
        case ValveState::Closed:
            return "Closed";
        case ValveState::Open:
            return "Open";
    }
}

inline static const char* toString(std::optional<ValveState> state) {
    if (!state) {
        return "Unknown";
    }
    return toString(*state);
}

struct IValve : virtual IPeripheral {
    /**
     * @brief Transition the valve to a new state.
     *
     * @param target The target state to transition to.
     *        If not specified, the valve will transition to its default state.
     * @return true if the state was changed, false if it was already in the target state.
     */
    virtual bool transitionTo(std::optional<TargetState> target) = 0;

    /**
     * @brief Get the current state of the valve.
     */
    virtual std::optional<ValveState> getState() const = 0;
};

}    // namespace cornucopia::ugly_duckling::peripherals::api

namespace ArduinoJson {

using cornucopia::ugly_duckling::peripherals::api::ValveState;

template <>
struct Converter<ValveState> {
    static void toJson(const ValveState src, JsonVariant dst) {
        switch (src) {
            case ValveState::Closed:
                dst.set("Closed");
                break;
            case ValveState::Open:
                dst.set("Open");
                break;
        }
    }

    static cornucopia::ugly_duckling::peripherals::api::ValveState fromJson(JsonVariantConst src) {
        auto* str = src.as<const char*>();
        if (strcmp(str, "Closed") == 0) {
            return cornucopia::ugly_duckling::peripherals::api::ValveState::Closed;
        } else {
            return cornucopia::ugly_duckling::peripherals::api::ValveState::Open;
        }
    }

    static bool checkJson(JsonVariantConst src) {
        if (!src.is<const char*>()) {
            return false;
        }
        auto* value = src.as<const char*>();
        return strcmp(value, "Closed") == 0 || strcmp(value, "Open") == 0;
    }
};

}    // namespace ArduinoJson
