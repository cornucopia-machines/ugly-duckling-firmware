#pragma once

#include <optional>

#include <ArduinoJson.h>

namespace cornucopia::ugly_duckling::peripherals::api {

enum class TargetState : int8_t {
    Closed = -1,
    Open = 1
};

inline static const char* toString(std::optional<TargetState> state) {
    if (!state.has_value()) {
        return "None";
    }
    switch (state.value()) {
        case TargetState::Closed:
            return "Closed";
        case TargetState::Open:
            return "Open";
        default:
            return "INVALID";
    }
}

}    // namespace cornucopia::ugly_duckling::peripherals::api

namespace ArduinoJson {

using cornucopia::ugly_duckling::peripherals::api::TargetState;

template <>
struct Converter<TargetState> {
    static void toJson(const TargetState src, JsonVariant dst) {
        switch (src) {
            case TargetState::Closed:
                dst.set("Closed");
                break;
            case TargetState::Open:
                dst.set("Open");
                break;
        }
    }

    static TargetState fromJson(JsonVariantConst src) {
        const auto* str = src.as<const char*>();
        return (strcmp(str, "Closed") == 0)
            ? TargetState::Closed
            : TargetState::Open;
    }

    static bool checkJson(JsonVariantConst src) {
        if (!src.is<const char*>()) {
            return false;
        }
        const auto* str = src.as<const char*>();
        return strcmp(str, "Closed") == 0 || strcmp(str, "Open") == 0;
    }
};
}    // namespace ArduinoJson
