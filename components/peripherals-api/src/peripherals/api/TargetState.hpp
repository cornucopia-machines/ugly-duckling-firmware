#pragma once

#include <optional>

#include <ArduinoJson.h>

namespace farmhub::peripherals::api {

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

}    // namespace farmhub::peripherals::api

namespace ArduinoJson {

using farmhub::peripherals::api::TargetState;

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

    static farmhub::peripherals::api::TargetState fromJson(JsonVariantConst src) {
        auto* str = src.as<const char*>();
        if (strcmp(str, "Closed") == 0) {
            return farmhub::peripherals::api::TargetState::Closed;
        } else {
            return farmhub::peripherals::api::TargetState::Open;
        }
    }

    static bool checkJson(JsonVariantConst src) {
        if (!src.is<const char*>()) {
            return false;
        }
        auto* str = src.as<const char*>();
        return strcmp(str, "Closed") == 0 || strcmp(str, "Open") == 0;
    }
};
}    // namespace ArduinoJson
