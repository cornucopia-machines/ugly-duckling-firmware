#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cornucopia::ugly_duckling::kernel {

/**
 * @brief The link the device reaches the server over.
 */
enum class NetworkLink : std::uint8_t {
    WiFi,
    Cellular,
};

inline const char* toString(NetworkLink link) {
    switch (link) {
        case NetworkLink::WiFi:
            return "wifi";
        case NetworkLink::Cellular:
            return "cellular";
    }
    return "unknown";
}

struct NetworkLinkChoice {
    NetworkLink link = NetworkLink::WiFi;
    // Why the configured links couldn't be used as given; the device falls back to WiFi then
    std::optional<std::string> error;
};

/**
 * @brief Picks the link to use from network-config's `links` (docs/specs/NB-IoT.md, "Choosing
 * WiFi or NB-IoT").
 *
 * `links` is an ordered list of preference, but only a single link is supported for now. No
 * `links` at all means WiFi, which is what every device used before the setting existed.
 * Anything that can't be honored falls back to WiFi with an error, rather than leaving the
 * device without a link.
 *
 * Pure decision function, no drivers -- unit-testable natively.
 *
 * @param cellularAvailable whether this build and board can talk to a cellular modem
 */
inline NetworkLinkChoice chooseNetworkLink(const std::vector<std::string>& links, bool cellularAvailable) {
    if (links.empty()) {
        return { .link = NetworkLink::WiFi, .error = std::nullopt };
    }
    if (links.size() > 1) {
        return { .link = NetworkLink::WiFi, .error = "only a single link is supported, got " + std::to_string(links.size()) };
    }
    const auto& link = links.front();
    if (link == toString(NetworkLink::WiFi)) {
        return { .link = NetworkLink::WiFi, .error = std::nullopt };
    }
    if (link == toString(NetworkLink::Cellular)) {
        if (!cellularAvailable) {
            return { .link = NetworkLink::WiFi, .error = "cellular link requested, but this device has no cellular modem" };
        }
        return { .link = NetworkLink::Cellular, .error = std::nullopt };
    }
    return { .link = NetworkLink::WiFi, .error = "unknown link '" + link + "'" };
}

}    // namespace cornucopia::ugly_duckling::kernel
