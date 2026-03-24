#ifdef FARMHUB_DEBUG
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
#include <devices/UglyDucklingMk9Rev1.hpp>

#elif defined(CONFIG_IDF_TARGET_ESP32C6)

#include <devices/UglyDucklingMk9Rev2.hpp>

#else
#error "Unsupported target"
#endif

#include <devices/GenericDevice.hpp>

using namespace farmhub::devices;
using namespace farmhub::kernel;

namespace {
void startDeviceBasedOnMac() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
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
    // TODO Use actual MAC address once devices ship
    if (macAddressHasPrefix(0xFF, 0xFF, 0xFF, 0xFF)) {
        startDevice<UglyDucklingMk9Rev1>();
        return;
    }

#elif defined(CONFIG_IDF_TARGET_ESP32C6)

    // MK9 Rev2
    // TODO Use actual MAC address once devices ship
    if (macAddressHasPrefix(0xFF, 0xFF, 0xFF, 0xFF)) {
        startDevice<UglyDucklingMk9Rev2>();
        return;
    }
#endif

    ESP_LOGW("device", "Unrecognized MAC address %s — falling back to generic device", getMacAddress().c_str());
    startDevice<GenericDevice>();
}
}    // namespace

extern "C" void app_main() {
#ifdef FARMHUB_DEBUG
    // Reset ANSI colors
    printf("\033[0m");
#endif

#if defined(MK5)
    startDevice<UglyDucklingMk5>();
#elif defined(MK6)
    startDevice<UglyDucklingMk6Rev3>();
#elif defined(MK6_REV1)
    startDevice<UglyDucklingMk6Rev1>();
#elif defined(MK6_REV2)
    startDevice<UglyDucklingMk6Rev2>();
#elif defined(MK7)
    startDevice<UglyDucklingMk7>();
#elif defined(MK8)
    startDevice<UglyDucklingMk8Rev2>();
#elif defined(MK8_REV1)
    startDevice<UglyDucklingMk8Rev1>();
#elif defined(MK9)
    startDevice<UglyDucklingMk9Rev2>();
#elif defined(MK9_REV1)
    startDevice<UglyDucklingMk9Rev1>();
#else
    startDeviceBasedOnMac();
#endif
}
