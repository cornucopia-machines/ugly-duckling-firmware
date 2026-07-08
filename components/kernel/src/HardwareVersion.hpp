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
constexpr uint16_t HW_EFUSE_FMT_VERSION = 0x0001;

struct __attribute__((packed)) HardwareIdentityRecord {
    uint16_t magic;
    uint16_t fmtVersion;
    uint16_t hwGen;
    uint16_t hwRev;
    uint16_t mfrId;
    uint64_t batch;    // manufacturer batch/lot ID (e.g. JLCPCB's base-36 code); 0 = not recorded
    uint64_t serial;
};

static_assert(sizeof(HardwareIdentityRecord) == 26);

struct HardwareVersion {
    uint16_t hwGen;
    uint16_t hwRev;
    uint16_t mfrId;
    uint64_t batch;
    uint64_t serial;
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
        .batch = identity.batch,
        .serial = identity.serial,
    };

    return version;
}

}    // namespace cornucopia::ugly_duckling::kernel
