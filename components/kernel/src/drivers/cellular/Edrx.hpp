#pragma once

#include <drivers/cellular/AtResponse.hpp>
#include <drivers/cellular/Rat.hpp>

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

// eDRX cycle lengths, indexed by their 4-bit code in AT+CEDRXS and friends (3GPP TS 24.008, table
// 10.5.5.32). In WB-S1 mode (LTE-M) every code has a cycle
inline constexpr std::array<milliseconds, 16> LTE_M_EDRX_CYCLES {
    5120ms, 10240ms, 20480ms, 40960ms, 61440ms, 81920ms, 102400ms, 122880ms,
    143360ms, 163840ms, 327680ms, 655360ms, 1310720ms, 2621440ms, 5242880ms, 10485760ms
};

// In NB-S1 mode (NB-IoT); 0 for the codes it doesn't have
inline constexpr std::array<milliseconds, 16> NB_IOT_EDRX_CYCLES {
    0ms, 0ms, 20480ms, 40960ms, 0ms, 81920ms, 0ms, 0ms,
    0ms, 163840ms, 327680ms, 655360ms, 1310720ms, 2621440ms, 5242880ms, 10485760ms
};

inline const std::array<milliseconds, 16>& edrxCycles(Rat rat) {
    return rat == Rat::LteM ? LTE_M_EDRX_CYCLES : NB_IOT_EDRX_CYCLES;
}

/**
 * @brief The <AcT-type> of AT+CEDRXS and friends: 4 for LTE-M (WB-S1), 5 for NB-IoT (NB-S1).
 */
inline int edrxAccessTechnology(Rat rat) {
    return rat == Rat::LteM ? 4 : 5;
}

/**
 * @brief The 4-bit code of an eDRX cycle, e.g. "0011" for 40960 ms.
 *
 * @return nullopt if the RAT has no such cycle
 */
inline std::optional<std::string> encodeEdrxCycle(Rat rat, milliseconds cycle) {
    const auto& cycles = edrxCycles(rat);
    for (size_t code = 0; code < cycles.size(); code++) {
        if (cycle > 0ms && cycles[code] == cycle) {
            return std::bitset<4>(code).to_string();
        }
    }
    return std::nullopt;
}

/**
 * @brief eDRX as the modem reports it in +CEDRXRDP and +CEDRXP.
 */
struct EdrxParameters {
    // Whether the cell uses eDRX for us at all; the cycles and window are only set when it does
    bool active;
    std::optional<Rat> rat;
    std::optional<milliseconds> requested;
    // What the network granted, which is what counts
    std::optional<milliseconds> granted;
    // How long the modem listens for paging in each cycle
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
    auto accessTechnology = (*fields)[0].asInt();
    // 0: the cell doesn't support eDRX
    std::optional<Rat> rat;
    if (accessTechnology == 4) {
        rat = Rat::LteM;
    } else if (accessTechnology == 5) {
        rat = Rat::NbIot;
    }
    EdrxParameters none { .active = false, .rat = rat, .requested = {}, .granted = {}, .pagingTimeWindow = {} };
    if (!rat) {
        return none;
    }
    // The 4-bit codes, as numbers; empty strings when not set
    auto code = [&](size_t index) -> std::optional<size_t> {
        auto value = index < fields->size() ? (*fields)[index].asBinary() : std::nullopt;
        return value && *value >= 0 && *value < 16 ? std::optional(static_cast<size_t>(*value)) : std::nullopt;
    };
    auto cycle = [&](size_t index) -> std::optional<milliseconds> {
        auto value = code(index);
        const auto& cycles = edrxCycles(*rat);
        return value && cycles[*value] > 0ms ? std::optional(cycles[*value]) : std::nullopt;
    };
    auto granted = cycle(2);
    // Without a granted cycle the modem pages on the cell's own cycle: +CEDRXRDP: 4,"","",""
    if (!granted) {
        return none;
    }
    auto window = code(3);
    // The paging time window counts in 1.28 s steps on LTE-M and 2.56 s on NB-IoT, from 1
    auto windowStep = *rat == Rat::LteM ? 1280ms : 2560ms;
    return EdrxParameters {
        .active = true,
        .rat = rat,
        .requested = cycle(1),
        .granted = granted,
        .pagingTimeWindow = window ? std::optional(windowStep * (*window + 1)) : std::nullopt,
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
 * @brief Parses the +CEDRXP URC the modem sends when the network changes the eDRX parameters.
 */
inline std::optional<EdrxParameters> parseCedrxp(std::string_view line) {
    return detail::parseEdrxFields(line, "+CEDRXP:");
}

/**
 * @brief Human-readable summary for the log, e.g. "81920 ms on LTE-M, paging window 2560 ms".
 */
inline std::string describe(const EdrxParameters& edrx) {
    if (!edrx.active) {
        return "not used on this cell";
    }
    auto ms = [](milliseconds duration) {
        return std::to_string(duration.count()) + " ms";
    };
    std::string description = edrx.granted ? ms(*edrx.granted) : "unknown cycle";
    if (edrx.rat) {
        description += std::string(" on ") + toString(*edrx.rat);
    }
    if (edrx.requested && edrx.requested != edrx.granted) {
        description += " (requested " + ms(*edrx.requested) + ")";
    }
    if (edrx.pagingTimeWindow) {
        description += ", paging window " + ms(*edrx.pagingTimeWindow);
    }
    return description;
}

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
