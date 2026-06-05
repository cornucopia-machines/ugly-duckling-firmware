#ifdef UD_DEBUG
#include <cstdio>
#endif

#include <esp_log.h>

#include <Device.hpp>
#include <MacAddress.hpp>

#if defined(CONFIG_IDF_TARGET_ESP32S3)

#include <devices/UglyDucklingMk5.hpp>
#include <devices/UglyDucklingMk6.hpp>
#include <devices/UglyDucklingMk7.hpp>
#include <devices/UglyDucklingMk8.hpp>
#include <devices/UglyDucklingMk9.hpp>

#elif defined(CONFIG_IDF_TARGET_ESP32C6)

#include <devices/UglyDucklingMk10.hpp>

#else
#error "Unsupported target"
#endif

#include <devices/GenericDevice.hpp>

using namespace cornucopia::ugly_duckling::devices;
using namespace cornucopia::ugly_duckling::kernel;

namespace {
void startDeviceBasedOnHardware() {
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
#else
    // MK10 Rev1
    if (macAddressHasPrefix(0xE8, 0xF6, 0x0A)) {
        startDevice<UglyDucklingMk10Rev1>();
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
