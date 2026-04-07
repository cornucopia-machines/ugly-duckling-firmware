#pragma once

#include <optional>
#include <utility>

#include <ArduinoJson.h>

#include "IPeripheral.hpp"
#include "TargetState.hpp"

namespace farmhub::peripherals::api {

enum class DoorState : int8_t {
    Closed = -1,
    Open = 1
};

inline static const char* toString(DoorState state) {
    switch (state) {
        case DoorState::Closed:
            return "Closed";
        case DoorState::Open:
            return "Open";
    }
    std::unreachable();
}

inline static const char* toString(std::optional<DoorState> state) {
    if (!state) {
        return "Unknown";
    }
    return toString(*state);
}

struct IDoor : virtual IPeripheral {
    /**
     * @brief Transition the door to a new state.
     *
     * @param target The target state to transition to. When unspecified, it stays in its current state.
     *                When current state is unspecified (`None`), transitions to `Closed`.
     * @return true if the state was changed, false if it was already in the target state.
     */
    virtual bool transitionTo(std::optional<TargetState> target) = 0;

    /**
     * @brief Get the current state of the door.
     */
    virtual std::optional<DoorState> getState() = 0;
};

}    // namespace farmhub::peripherals::api

namespace ArduinoJson {

using farmhub::peripherals::api::DoorState;

template <>
struct Converter<DoorState> {
    static bool toJson(const DoorState src, JsonVariant dst) {
        return dst.set(static_cast<int>(src));
    }

    static DoorState fromJson(JsonVariantConst src) {
        return static_cast<DoorState>(src.as<int>());
    }

    static bool checkJson(JsonVariantConst src) {
        return src.is<int>();
    }
};

}    // namespace ArduinoJson
