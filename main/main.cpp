#ifdef FARMHUB_DEBUG
#include <cstdio>
#endif

#include <cstdlib>

#include <esp_log.h>
#include <esp_system.h>

#include <Device.hpp>
#include <MacAddress.hpp>

#ifdef CONFIG_IDF_TARGET_ESP32S3
#include <devices/UglyDucklingMk5.hpp>
#include <devices/UglyDucklingMk6.hpp>
#include <devices/UglyDucklingMk7.hpp>
#include <devices/UglyDucklingMk8.hpp>
#endif

#ifdef CONFIG_IDF_TARGET_ESP32C6
#include <devices/UglyDucklingMkX.hpp>
#endif

using namespace farmhub::devices;
using namespace farmhub::kernel;

#ifdef CONFIG_IDF_TARGET_ESP32S3
namespace {
void dispatchToDevice() {
    // MK5 Rev2
    if (macAddressHasPrefix(0xF4, 0x12, 0xFA, 0x52)) {
        startDevice<Mk5Settings, UglyDucklingMk5>();
        return;
    }

    // MK6 Rev1
    if (macAddressHasPrefix(0x34, 0x85, 0x18)) {
        startDevice<Mk6Settings, UglyDucklingMk6Rev1>();
        return;
    }

    // MK6 Rev2
    if (macAddressHasPrefix(0xEC, 0xDA, 0x3B, 0x5B)) {
        startDevice<Mk6Settings, UglyDucklingMk6Rev2>();
        return;
    }

    // MK6 Rev3
    if (macAddressHasPrefix(0x98, 0xA3, 0x16, 0x1A)) {
        startDevice<Mk6Settings, UglyDucklingMk6Rev3>();
        return;
    }

    // MK7 Rev1
    if (macAddressHasPrefix(0x48, 0x27, 0xE2, 0x82)) {
        startDevice<Mk7Settings, UglyDucklingMk7>();
        return;
    }

    // MK8 Rev1
    if (macAddressHasPrefix(0x98, 0xA3, 0x16, 0x1A)) {
        startDevice<Mk8Settings, UglyDucklingMk8Rev1>();
        return;
    }

    ESP_LOGE("device", "Unrecognized MAC address %s — cannot select device variant", getMacAddress().c_str());
    esp_system_abort("Unrecognized MAC address");
}
}    // namespace
#endif

#ifdef CONFIG_IDF_TARGET_ESP32C6
namespace {
void dispatchToDevice() {
    // MKX — TODO: add actual production MAC range
    if (macAddressHasPrefix()) {
        startDevice<MkXSettings, UglyDucklingMkX>();
        return;
    }

    ESP_LOGE("device", "Unrecognized MAC address %s — cannot select device variant", getMacAddress().c_str());
    esp_system_abort("Unrecognized MAC address");
}
}    // namespace
#endif

extern "C" void app_main() {
#ifdef FARMHUB_DEBUG
    // Reset ANSI colors
    printf("\033[0m");
#endif

#if defined(MK5)
    startDevice<Mk5Settings, UglyDucklingMk5>();
#elif defined(MK6)
    startDevice<Mk6Settings, UglyDucklingMk6Rev3>();
#elif defined(MK6_REV1)
    startDevice<Mk6Settings, UglyDucklingMk6Rev1>();
#elif defined(MK6_REV2)
    startDevice<Mk6Settings, UglyDucklingMk6Rev2>();
#elif defined(MK7)
    startDevice<Mk7Settings, UglyDucklingMk7>();
#elif defined(MK8)
    startDevice<Mk8Settings, UglyDucklingMk8Rev2>();
#elif defined(MK8_REV1)
    startDevice<Mk8Settings, UglyDucklingMk8Rev1>();
#elif defined(MKX)
    startDevice<MkXSettings, UglyDucklingMkX>();
#else
    dispatchToDevice();
#endif
}
