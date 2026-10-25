#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

namespace cornucopia::ugly_duckling::kernel::config {

/**
 * @brief One of the two interchangeable full configuration sets (device + every function) a
 * device holds in NVS at any time (docs/Configuration.md, "Storage: envelopes and slots").
 * `config-state` points `confirmed` (and, while staging, `requested`) at one of these.
 */
enum class ConfigSlot : std::uint8_t {
    A,
    B
};

inline std::string toString(ConfigSlot slot) {
    return slot == ConfigSlot::B ? "b" : "a";
}

inline ConfigSlot otherSlot(ConfigSlot slot) {
    return slot == ConfigSlot::A ? ConfigSlot::B : ConfigSlot::A;
}

/**
 * @brief Where a staged `requested` set is in the apply/commit/revert flow
 * (docs/Configuration.md, "Storage: envelopes and slots" and "The confirmed/requested state
 * machine").
 */
enum class RequestedConfigStatus : std::uint8_t {
    Pending,
    Attempted,
    Rejected
};

/**
 * @brief The `google.rpc.Code` subset the firmware can emit (docs/Configuration.md,
 * "Rejection reporting"). `OK` is never stored here -- a matching fingerprint *is* success, so there is
 * nothing to reject.
 */
enum class RejectionCode : std::uint8_t {
    InvalidArgument = 3,
    ResourceExhausted = 8,
    FailedPrecondition = 9,
    Unimplemented = 12,
    Internal = 13,
};

/**
 * @brief A staged-but-not-yet-confirmed configuration set and its apply status.
 */
struct RequestedConfig {
    ConfigSlot slot;
    RequestedConfigStatus status;
};

/**
 * @brief The device's whole reconciliation state: which slot is last-known-good, which (if any)
 * is staged and how far it got, and any rejection still awaiting report on the next `BOOT` (and
 * that boot's next `SYNC`).
 * Default-constructed (every field absent) represents a virgin `config-state` namespace (fresh
 * device or pre-Phase-3 firmware). `loadDeviceBootConfig()` guarantees `confirmed` is always set
 * before any downstream code runs -- a missing confirmed is initialized to slot A at boot (see
 * BootConfig.hpp), so downstream code never sees `confirmed == nullopt`.
 */
struct ConfigState {
    std::optional<ConfigSlot> confirmed;
    std::optional<RequestedConfig> requested;
    std::optional<RejectionCode> rejection;
};

}    // namespace cornucopia::ugly_duckling::kernel::config

namespace ArduinoJson {

using cornucopia::ugly_duckling::kernel::config::ConfigSlot;
using cornucopia::ugly_duckling::kernel::config::ConfigState;
using cornucopia::ugly_duckling::kernel::config::RejectionCode;
using cornucopia::ugly_duckling::kernel::config::RequestedConfig;
using cornucopia::ugly_duckling::kernel::config::RequestedConfigStatus;

template <>
struct Converter<ConfigSlot> {
    static void toJson(ConfigSlot src, JsonVariant dst) {
        dst.set(src == ConfigSlot::B ? "b" : "a");
    }

    static ConfigSlot fromJson(JsonVariantConst src) {
        const auto* str = src.as<const char*>();
        return (str != nullptr && strcmp(str, "b") == 0) ? ConfigSlot::B : ConfigSlot::A;
    }

    static bool checkJson(JsonVariantConst src) {
        return src.is<const char*>();
    }
};

template <>
struct Converter<RequestedConfigStatus> {
    static void toJson(RequestedConfigStatus src, JsonVariant dst) {
        switch (src) {
            case RequestedConfigStatus::Attempted:
                dst.set("attempted");
                break;
            case RequestedConfigStatus::Rejected:
                dst.set("rejected");
                break;
            case RequestedConfigStatus::Pending:
            default:
                dst.set("pending");
                break;
        }
    }

    static RequestedConfigStatus fromJson(JsonVariantConst src) {
        const auto* str = src.as<const char*>();
        if (str != nullptr && strcmp(str, "attempted") == 0) {
            return RequestedConfigStatus::Attempted;
        }
        if (str != nullptr && strcmp(str, "rejected") == 0) {
            return RequestedConfigStatus::Rejected;
        }
        return RequestedConfigStatus::Pending;
    }

    static bool checkJson(JsonVariantConst src) {
        return src.is<const char*>();
    }
};

template <>
struct Converter<RejectionCode> {
    static void toJson(RejectionCode src, JsonVariant dst) {
        dst.set(static_cast<std::int32_t>(src));
    }

    static RejectionCode fromJson(JsonVariantConst src) {
        auto value = src.as<std::uint8_t>();
        if (value == 0) {
            return RejectionCode::Internal;
        }
        return static_cast<RejectionCode>(value);
    }

    static bool checkJson(JsonVariantConst src) {
        return src.is<std::uint8_t>();
    }
};

template <>
struct Converter<RequestedConfig> {
    static bool toJson(const RequestedConfig& src, JsonVariant dst) {
        JsonObject obj = dst.to<JsonObject>();
        obj["slot"] = src.slot;
        obj["status"] = src.status;
        return true;
    }

    static RequestedConfig fromJson(JsonVariantConst src) {
        return { .slot = src["slot"].as<ConfigSlot>(), .status = src["status"].as<RequestedConfigStatus>() };
    }

    static bool checkJson(JsonVariantConst src) {
        return src["slot"].is<const char*>() && src["status"].is<const char*>();
    }
};

template <>
struct Converter<ConfigState> {
    static bool toJson(const ConfigState& src, JsonVariant dst) {
        JsonObject obj = dst.to<JsonObject>();
        if (src.confirmed) {
            obj["confirmed"] = *src.confirmed;
        }
        if (src.requested) {
            obj["requested"] = *src.requested;
        }
        if (src.rejection) {
            obj["rejection"] = *src.rejection;
        }
        return true;
    }

    static ConfigState fromJson(JsonVariantConst src) {
        ConfigState state;
        if (src["confirmed"].is<const char*>()) {
            state.confirmed = src["confirmed"].as<ConfigSlot>();
        }
        if (src["requested"].is<JsonObjectConst>()) {
            state.requested = src["requested"].as<RequestedConfig>();
        }
        if (src["rejection"].is<std::int32_t>()) {
            state.rejection = src["rejection"].as<RejectionCode>();
        }
        return state;
    }

    static bool checkJson(JsonVariantConst src) {
        return src.is<JsonObjectConst>();
    }
};

}    // namespace ArduinoJson
