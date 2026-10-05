#pragma once

#include <drivers/cellular/AtResponse.hpp>

#include <array>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

using namespace std::chrono;
using namespace std::chrono_literals;

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

// eDRX cycle lengths in NB-S1 mode, indexed by their 4-bit code in AT+CEDRXS and friends (3GPP TS
// 24.008, table 10.5.5.32); 0 for the codes only LTE-M (WB-S1) has
inline constexpr std::array<milliseconds, 16> EDRX_CYCLES {
    0ms, 0ms, 20480ms, 40960ms, 0ms, 81920ms, 0ms, 0ms,
    0ms, 163840ms, 327680ms, 655360ms, 1310720ms, 2621440ms, 5242880ms, 10485760ms
};

/**
 * @brief The 4-bit code of an eDRX cycle, e.g. "0011" for 40960 ms.
 *
 * @return nullopt if NB-IoT has no such cycle
 */
inline std::optional<std::string> encodeEdrxCycle(milliseconds cycle) {
    for (size_t code = 0; code < EDRX_CYCLES.size(); code++) {
        if (cycle > 0ms && EDRX_CYCLES[code] == cycle) {
            return std::bitset<4>(code).to_string();
        }
    }
    return std::nullopt;
}

/**
 * @brief eDRX as the module reports it in +CEDRXRDP and +CEDRXP.
 */
struct EdrxParameters {
    // Whether the cell uses eDRX for us at all; the rest is only set when it does
    bool active;
    std::optional<milliseconds> requested;
    // What the network granted, which is what counts
    std::optional<milliseconds> granted;
    // How long the module listens for paging in each cycle
    std::optional<milliseconds> pagingTimeWindow;

    bool operator==(const EdrxParameters&) const = default;
};

namespace detail {

// <AcT_type>[,<requested_eDRX_value>[,<NW_provided_eDRX_value>[,<paging_time_window>]]]
inline std::optional<EdrxParameters> parseEdrxFields(std::string_view line, std::string_view prefix) {
    auto fields = parseAtFields(line, prefix);
    if (!fields || fields->empty() || !(*fields)[0].asInt()) {
        return std::nullopt;
    }
    // 0: the cell doesn't support eDRX; 5: NB-S1 mode
    if ((*fields)[0].asInt() != 5) {
        return EdrxParameters { .active = false, .requested = {}, .granted = {}, .pagingTimeWindow = {} };
    }
    // The 4-bit codes, as numbers
    auto code = [&](size_t index) -> std::optional<size_t> {
        auto value = index < fields->size() ? (*fields)[index].asBinary() : std::nullopt;
        return value && *value >= 0 && *value < 16 ? std::optional(static_cast<size_t>(*value)) : std::nullopt;
    };
    auto cycle = [&](size_t index) -> std::optional<milliseconds> {
        auto value = code(index);
        return value && EDRX_CYCLES[*value] > 0ms ? std::optional(EDRX_CYCLES[*value]) : std::nullopt;
    };
    auto window = code(3);
    return EdrxParameters {
        .active = true,
        .requested = cycle(1),
        .granted = cycle(2),
        // 2.56 s times the code plus one
        .pagingTimeWindow = window ? std::optional(2560ms * (*window + 1)) : std::nullopt,
    };
}

}    // namespace detail

/**
 * @brief Parses the response to AT+CEDRXRDP: what the network granted on the current cell.
 */
inline std::optional<EdrxParameters> parseCedrxrdp(std::string_view line) {
    return detail::parseEdrxFields(line, "+CEDRXRDP:");
}

/**
 * @brief Parses the +CEDRXP URC the module sends when the network changes the eDRX parameters.
 */
inline std::optional<EdrxParameters> parseCedrxp(std::string_view line) {
    return detail::parseEdrxFields(line, "+CEDRXP:");
}

/**
 * @brief Parses the response to the BC660K's AT+QDRX?: +QDRX: <mode>[,<drxcycle_IDLE>] while idle,
 * or +QDRX: 2,... with connected mode DRX timers.
 *
 * @return the idle mode paging (DRX) cycle; nullopt while connected, or with no cycle reported
 */
inline std::optional<milliseconds> parseQdrxIdleCycle(std::string_view line) {
    auto fields = parseAtFields(line, "+QDRX:");
    // Mode 1 is RRC idle
    if (!fields || fields->size() < 2 || (*fields)[0].asInt() != 1) {
        return std::nullopt;
    }
    auto cycle = (*fields)[1].asInt();
    return cycle && *cycle > 0 ? std::optional(milliseconds(*cycle)) : std::nullopt;
}

/**
 * @brief Human-readable summary for the log, e.g. "40960 ms (requested 81920 ms), paging window 2560 ms".
 */
inline std::string describe(const EdrxParameters& edrx) {
    if (!edrx.active) {
        return "not used on this cell";
    }
    auto ms = [](milliseconds duration) {
        return std::to_string(duration.count()) + " ms";
    };
    std::string description = edrx.granted ? ms(*edrx.granted) : "unknown cycle";
    if (edrx.requested && edrx.requested != edrx.granted) {
        description += " (requested " + ms(*edrx.requested) + ")";
    }
    if (edrx.pagingTimeWindow) {
        description += ", paging window " + ms(*edrx.pagingTimeWindow);
    }
    return description;
}

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
