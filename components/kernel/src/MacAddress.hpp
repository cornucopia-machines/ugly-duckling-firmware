#pragma once

#include <esp_err.h>
#include <esp_mac.h>

#include <string>
#include <vector>

namespace cornucopia::ugly_duckling::kernel {

constexpr size_t MAC_ADDRESS_LENGTH = 6;

inline std::array<uint8_t, MAC_ADDRESS_LENGTH> getRawMacAddress() {
    static bool queried;
    static std::array<uint8_t, MAC_ADDRESS_LENGTH> mac {};
    if (!queried) {
        ESP_ERROR_CHECK(esp_read_mac(mac.data(), ESP_MAC_WIFI_STA));
        queried = true;
    }
    return mac;
}

inline const std::string& getMacAddress() {
    static std::string macAddress;
    if (macAddress.empty()) {
        auto rawMac = getRawMacAddress();
        char mac[4 * MAC_ADDRESS_LENGTH];    // "xx:xx:xx:xx:xx:xx" + null terminator
        (void) sprintf(mac, "%02x:%02x:%02x:%02x:%02x:%02x",
            rawMac[0], rawMac[1], rawMac[2], rawMac[3],
            rawMac[4], rawMac[5]);
        macAddress = mac;
    }
    return macAddress;
}

template <typename... Bytes>
[[maybe_unused]]
static bool macAddressHasPrefix(Bytes... bytes) {
    const auto mac = getRawMacAddress();
    const std::array<uint8_t, sizeof...(Bytes)> prefix { static_cast<uint8_t>(bytes)... };
    return std::equal(prefix.begin(), prefix.end(), mac.begin());
}

}    // namespace cornucopia::ugly_duckling::kernel
