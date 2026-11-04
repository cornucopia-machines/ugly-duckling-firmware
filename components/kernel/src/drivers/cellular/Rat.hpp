#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

/**
 * @brief The radio access technologies the nRF9151 has: LTE-M (Cat-M1) and NB-IoT (Cat-NB).
 */
enum class Rat : uint8_t {
    LteM,
    NbIot,
};

/**
 * @brief The name network-config uses: "lte-m" or "nb-iot".
 */
inline const char* toConfigName(Rat rat) {
    return rat == Rat::LteM ? "lte-m" : "nb-iot";
}

inline const char* toString(Rat rat) {
    return rat == Rat::LteM ? "LTE-M" : "NB-IoT";
}

inline std::optional<Rat> parseRat(std::string_view name) {
    if (name == "lte-m") {
        return Rat::LteM;
    }
    if (name == "nb-iot") {
        return Rat::NbIot;
    }
    return std::nullopt;
}

/**
 * @brief The RATs network-config lists (CellularConfig::rats), in order, without unknown names
 * and repeats; both, LTE-M first, when it lists none it knows.
 */
inline std::vector<Rat> parseRats(const std::vector<std::string>& names) {
    std::vector<Rat> rats;
    for (const auto& name : names) {
        auto rat = parseRat(name);
        if (rat && std::ranges::find(rats, *rat) == rats.end()) {
            rats.push_back(*rat);
        }
    }
    if (rats.empty()) {
        return { Rat::LteM, Rat::NbIot };
    }
    return rats;
}

/**
 * @brief The RAT of an <AcT> in +CEREG, %XMONITOR and +COPS: 7 is LTE-M, 9 is NB-IoT.
 */
inline std::optional<Rat> fromAccessTechnology(std::optional<int> accessTechnology) {
    if (accessTechnology == 7) {
        return Rat::LteM;
    }
    if (accessTechnology == 9) {
        return Rat::NbIot;
    }
    return std::nullopt;
}

/**
 * @brief The %XSYSTEMMODE setting for the RATs to use, in order of preference:
 * "<LTE-M>,<NB-IoT>,<GNSS>,<preference>". GNSS stays off.
 *
 * Preference 1 is LTE-M first, 2 NB-IoT first; with only one RAT enabled it's 0, none.
 *
 * @return nullopt for an empty list
 */
inline std::optional<std::string> toSystemMode(const std::vector<Rat>& rats) {
    if (rats.empty()) {
        return std::nullopt;
    }
    bool lteM = false;
    bool nbIot = false;
    for (auto rat : rats) {
        (rat == Rat::LteM ? lteM : nbIot) = true;
    }
    int preference = 0;
    if (lteM && nbIot) {
        preference = rats.front() == Rat::LteM ? 1 : 2;
    }
    return std::string(lteM ? "1" : "0") + "," + (nbIot ? "1" : "0") + ",0," + std::to_string(preference);
}

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
