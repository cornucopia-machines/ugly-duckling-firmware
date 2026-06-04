#pragma once

#include <cstdint>
#include <string>

namespace cornucopia::ugly_duckling::kernel::drivers {

struct WifiApRecord {
    std::string ssid;
    int8_t rssi;
    std::string authMode;
    uint8_t wifiGen;    // highest 802.11 generation supported: 1=b, 3=g, 4=n, 6=ax
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
