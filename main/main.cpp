#ifdef UD_DEBUG
#include <cstdio>
#endif

#include <esp_log.h>

#include <Device.hpp>
#include <HardwareVersion.hpp>
#include <MacAddress.hpp>

#if defined(CONFIG_IDF_TARGET_ESP32S3)

#include <devices/UglyDucklingMk5.hpp>
#include <devices/UglyDucklingMk6.hpp>
#include <devices/UglyDucklingMk7.hpp>
#include <devices/UglyDucklingMk8.hpp>
#include <devices/UglyDucklingMk9.hpp>

#elif defined(CONFIG_IDF_TARGET_ESP32C6)

#include <devices/UglyDucklingMk10.hpp>
#include <devices/UglyDucklingMk11.hpp>

#else
#error "Unsupported target"
#endif

#include <devices/GenericDevice.hpp>

using namespace cornucopia::ugly_duckling::devices;
using namespace cornucopia::ugly_duckling::kernel;

namespace {
void startDeviceBasedOnHardware() {
    const auto& hardwareVersion = getHardwareVersion();
    if (hardwareVersion.has_value()) {
        ESP_LOGI("device", "Hardware identity (eFuse): generation %d, revision %d, manufacturer 0x%02x, serial %lu",
            hardwareVersion->hwGen, hardwareVersion->hwRev, hardwareVersion->mfrId,
            static_cast<unsigned long>(hardwareVersion->serial));
    } else {
        ESP_LOGI("device", "No hardware identity eFuse record found — hardware version is unknown (expected for MK10 and earlier)");
    }

#if defined(CONFIG_IDF_TARGET_ESP32S3)
#if defined(MK5_REV2)
    startDevice<UglyDucklingMk5>();
#elif defined(MK6_REV1)
    startDevice<UglyDucklingMk6Rev1>();
#elif defined(MK6_REV2)
    startDevice<UglyDucklingMk6Rev2>();
#elif defined(MK6_REV3)
    startDevice<UglyDucklingMk6Rev3>();
#elif defined(MK7_REV1)
    startDevice<UglyDucklingMk7>();
#elif defined(MK8_REV1)
    startDevice<UglyDucklingMk8Rev1>();
#elif defined(MK9_REV1)
    startDevice<UglyDucklingMk9Rev1>();
#else
    // MK5 Rev2
    if (macAddressHasPrefix(0xF4, 0x12, 0xFA, 0x52)) {
        startDevice<UglyDucklingMk5>();
        return;
    }

    // MK6 Rev1
    if (macAddressHasPrefix(0x34, 0x85, 0x18)) {
        startDevice<UglyDucklingMk6Rev1>();
        return;
    }

    // MK6 Rev2
    if (macAddressHasPrefix(0xEC, 0xDA, 0x3B, 0x5B)) {
        startDevice<UglyDucklingMk6Rev2>();
        return;
    }

    // MK6 Rev3
    if (macAddressHasPrefix(0xF0, 0x9E, 0x9E, 0x55)) {
        startDevice<UglyDucklingMk6Rev3>();
        return;
    }

    // MK7 Rev1
    if (macAddressHasPrefix(0x48, 0x27, 0xE2, 0x82)) {
        startDevice<UglyDucklingMk7>();
        return;
    }

    // MK8 Rev1
    if (macAddressHasPrefix(0x98, 0xA3, 0x16, 0x1A)) {
        startDevice<UglyDucklingMk8Rev1>();
        return;
    }

    // MK9 Rev1
    if (macAddressHasPrefix(0x58, 0xE6, 0xC5, 0x41)
        || macAddressHasPrefix(0x58, 0xE6, 0xC5, 0x42)) {
        startDevice<UglyDucklingMk9Rev1>();
        return;
    }
#endif
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
#if defined(MK10_REV1)
    startDevice<UglyDucklingMk10Rev1>();
#elif defined(MK11_REV1)
    startDevice<UglyDucklingMk11Rev1>();
#else
    // Prefer the eFuse-burned hardware identity over MAC matching when it's
    // present — it's authoritative by construction, unlike MAC prefixes,
    // which can't reliably distinguish MK10 from newer MK11 batches. See
    // docs/specs/Hardware-Version-in-eFuse.md. hw_rev is 0-indexed (0 = first
    // release of the generation), hence hw_rev == 0 for each *Rev1 class below.
    if (hardwareVersion.has_value()) {
        // MK10 Rev1
        if (hardwareVersion->hwGen == 10 && hardwareVersion->hwRev == 0) {
            startDevice<UglyDucklingMk10Rev1>();
            return;
        }

        // MK11 Rev1
        if (hardwareVersion->hwGen == 11 && hardwareVersion->hwRev == 0) {
            startDevice<UglyDucklingMk11Rev1>();
            return;
        }

        ESP_LOGW("device", "Unrecognized hardware identity (generation %d, revision %d) — falling back to MAC-based detection",
            hardwareVersion->hwGen, hardwareVersion->hwRev);
    }

    // MAC-based fallback for boards without a burned hardware identity (all MK10s, and MK11s built before board test started burning eFuse)

    // MK10 Rev1
    if (macAddressHasPrefix(0xE8, 0xF6, 0x0A)) {
        startDevice<UglyDucklingMk10Rev1>();
        return;
    }

    // MK11 Rev1 -- 9c:cc:01
    if (macAddressHasPrefix(0x9C, 0xCC, 0x01)) {
        startDevice<UglyDucklingMk11Rev1>();
        return;
    }
#endif
#endif

    ESP_LOGW("device", "Unrecognized MAC address %s — falling back to generic device", getMacAddress().c_str());
    startDevice<GenericDevice>();
}
}    // namespace

extern "C" void app_main() {
#ifdef UD_DEBUG
    // Reset ANSI colors
    printf("\033[0m");
#endif

    startDeviceBasedOnHardware();
}
