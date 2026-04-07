#pragma once

#include <string>
#include <vector>

#include <esp_mac.h>

namespace farmhub::kernel {

constexpr size_t MAC_ADDRESS_LENGTH = 6;

static std::array<uint8_t, MAC_ADDRESS_LENGTH> getRawMacAddress() {
    static bool queried;
    static std::array<uint8_t, MAC_ADDRESS_LENGTH> mac {};
    if (!queried) {
        ESP_ERROR_THROW(esp_read_mac(mac.data(), ESP_MAC_WIFI_STA));
        queried = true;
    }
    return mac;
}

static const std::string& getMacAddress() {
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

}    // namespace farmhub::kernel
