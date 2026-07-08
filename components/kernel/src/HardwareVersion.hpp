#pragma once

#include <cstdint>
#include <cstring>
#include <optional>

#include <esp_efuse.h>
#include <esp_efuse_table.h>

#include <Log.hpp>

namespace cornucopia::ugly_duckling::kernel {

// See docs/specs/Hardware-Version-in-eFuse.md for the full design rationale.

constexpr uint16_t HW_EFUSE_MAGIC = 0x5544;    // 'UD'
constexpr uint8_t HW_EFUSE_FMT_VERSION = 0x01;

struct __attribute__((packed)) HardwareIdentityRecord {
    uint16_t magic;
    uint8_t fmtVersion;
    uint8_t hwGen;
    uint8_t hwRev;
    uint8_t mfrId;
    uint32_t serial;
};

static_assert(sizeof(HardwareIdentityRecord) == 10);

struct HardwareVersion {
    uint8_t hwGen;
    uint8_t hwRev;
    uint8_t mfrId;
    uint32_t serial;
};

// Returns std::nullopt if the eFuse record is unburned (expected for MK10 and
// earlier), unreadable, or fails its magic/format check.
static const std::optional<HardwareVersion>& getHardwareVersion() {
    static bool queried = false;
    static std::optional<HardwareVersion> version;
    if (queried) {
        return version;
    }
    queried = true;

    HardwareIdentityRecord identity {};
    auto err = esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, &identity, sizeof(identity) * 8);
    if (err != ESP_OK) {
        LOGW("Failed to read hardware identity eFuse record: %s", esp_err_to_name(err));
        return version;
    }
    if (identity.magic != HW_EFUSE_MAGIC || identity.fmtVersion != HW_EFUSE_FMT_VERSION) {
        LOGW("Hardware identity eFuse record not present (magic 0x%04x, fmt %d) — treating hardware version as unknown",
            identity.magic, identity.fmtVersion);
        return version;
    }

    version = HardwareVersion {
        .hwGen = identity.hwGen,
        .hwRev = identity.hwRev,
        .mfrId = identity.mfrId,
        .serial = identity.serial,
    };

    return version;
}

}    // namespace cornucopia::ugly_duckling::kernel
