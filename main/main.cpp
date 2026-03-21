#ifdef FARMHUB_DEBUG
#include <cstdio>
#endif

#include <array>
#include <cstdint>
#include <cstdlib>

#include <esp_log.h>

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
    // MK6 Rev1 — MAC prefix 0x34:0x85:0x18
    if (macAddressMatchesAny(std::array<uint8_t, 3> { 0x34, 0x85, 0x18 })) {
        startDevice<Mk6Settings, UglyDucklingMk6Rev1>();
        return;
    }

    // MK6 Rev2 — MAC prefix 0xec:0xda:0x3b:0x5b
    if (macAddressMatchesAny(std::array<uint8_t, 4> { 0xec, 0xda, 0x3b, 0x5b })) {
        startDevice<Mk6Settings, UglyDucklingMk6Rev2>();
        return;
    }

    // MK6 Rev3 — TODO: replace dummy ranges with actual production MAC ranges
    if (macAddressMatchesAny(
            std::array<uint8_t, 2> { 0xAA, 0x01 },
            std::array<uint8_t, 3> { 0xAA, 0x02, 0x03 })) {
        startDevice<Mk6Settings, UglyDucklingMk6Rev3>();
        return;
    }

    // MK5 — TODO: replace dummy range with actual production MAC range
    if (macAddressMatchesAny(std::array<uint8_t, 2> { 0xAA, 0x00 })) {
        startDevice<Mk5Settings, UglyDucklingMk5>();
        return;
    }

    // MK7 — TODO: replace dummy range with actual production MAC range
    if (macAddressMatchesAny(std::array<uint8_t, 2> { 0xAA, 0x04 })) {
        startDevice<Mk7Settings, UglyDucklingMk7>();
        return;
    }

    // MK8 Rev1 — MAC prefix 0x98:0xa3:0x16:0x1a
    if (macAddressMatchesAny(std::array<uint8_t, 4> { 0x98, 0xa3, 0x16, 0x1a })) {
        startDevice<Mk8Settings, UglyDucklingMk8Rev1>();
        return;
    }

    // MK8 Rev2 — TODO: replace dummy range with actual production MAC range
    if (macAddressMatchesAny(std::array<uint8_t, 2> { 0xAA, 0x05 })) {
        startDevice<Mk8Settings, UglyDucklingMk8Rev2>();
        return;
    }

    ESP_LOGE("device", "Unrecognized MAC address %s — cannot select device variant", getMacAddress().c_str());
    abort();
}
}    // namespace
#endif

#ifdef CONFIG_IDF_TARGET_ESP32C6
namespace {
void dispatchToDevice() {
    // MKX — TODO: replace dummy range with actual production MAC range
    if (macAddressMatchesAny(std::array<uint8_t, 2> { 0xAA, 0x10 })) {
        startDevice<MkXSettings, UglyDucklingMkX>();
        return;
    }

    ESP_LOGE("device", "Unrecognized MAC address %s — cannot select device variant", getMacAddress().c_str());
    abort();
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
