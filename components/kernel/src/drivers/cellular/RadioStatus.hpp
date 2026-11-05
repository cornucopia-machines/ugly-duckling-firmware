#pragma once

#include <drivers/cellular/AtResponse.hpp>
#include <drivers/cellular/Rat.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

namespace detail {

// The RRC state is the last field: "+CSCON: <mode>" as a URC, "+CSCON: <n>,<mode>" when read
inline std::optional<bool> parseCscon(std::string_view line, size_t fieldCount) {
    auto fields = parseAtFields(line, "+CSCON:");
    if (!fields || fields->size() != fieldCount) {
        return std::nullopt;
    }
    auto mode = fields->back().asInt();
    return mode == 0 || mode == 1 ? std::optional<bool>(mode == 1) : std::nullopt;
}

// RSRP in dBm from the nRF91's index: 1 is -140 dBm, up to 97 for -44 dBm; 255 is unknown
inline std::optional<int> rsrpFromIndex(std::optional<int> index) {
    return index && *index >= 0 && *index <= 97 ? std::optional(*index - 141) : std::nullopt;
}

// SNR in dB from the nRF91's index: 1 is -24 dB, up to 49 for 24 dB; 127 is unknown
inline std::optional<int> snrFromIndex(std::optional<int> index) {
    return index && *index >= 0 && *index <= 49 ? std::optional(*index - 25) : std::nullopt;
}

// RSRQ in dB from the nRF91's index: 0.5 dB steps, 1 is -19.5 dB, up to 34 for -3 dB; 255 is
// unknown. Like RSRP, the bottom of the index's range, here rounded down to whole dB
inline std::optional<int> rsrqFromIndex(std::optional<int> index) {
    return index && *index >= 0 && *index <= 34 ? std::optional(static_cast<int>(std::floor(-20.0 + (*index * 0.5)))) : std::nullopt;
}

}    // namespace detail

/**
 * @brief Parses the +CSCON: <mode> URC the modem sends when the RRC connection goes up or down.
 *
 * @return true when connected, false when idle
 */
inline std::optional<bool> parseCsconUrc(std::string_view line) {
    return detail::parseCscon(line, 1);
}

/**
 * @brief Parses the response to AT+CSCON?: +CSCON: <n>,<mode>.
 *
 * @return true when connected, false when idle
 */
inline std::optional<bool> parseCsconRead(std::string_view line) {
    return detail::parseCscon(line, 2);
}

/**
 * @brief The serving cell's radio state, from %XMONITOR, with what %CONEVAL adds.
 */
struct ServingCell {
    // Empty while not camped on a cell
    std::string cellId;
    std::optional<Rat> rat;
    // The operator's MCC and MNC, e.g. "21670", and its name if the network sent one
    std::string plmn;
    std::string operatorName;
    std::string trackingAreaCode;
    int band = 0;
    int earfcn = 0;
    int pci = 0;
    // dBm, dB, dB
    std::optional<int> rsrp;
    std::optional<int> rsrq;
    std::optional<int> sinr;
    // Coverage enhancement level 0-3, from %CONEVAL
    std::optional<int> ecl;
    // dBm, from %CONEVAL: the power the modem estimates it would transmit at
    std::optional<int> txPower;

    /**
     * @brief Whether the modem has a serving cell at all.
     */
    bool isCamped() const {
        return !cellId.empty();
    }
};

/**
 * @brief Parses the response to AT%XMONITOR.
 *
 * %XMONITOR: <reg_status>[,<full_name>,<short_name>,<plmn>,<tac>,<AcT>,<band>,<cell_id>,
 * <phys_cell_id>,<EARFCN>,<rsrp>,<snr>,<NW-provided_eDRX_value>,<Active-Time>,
 * <Periodic-TAU-ext>,<Periodic-TAU>]. Without a cell, only the registration status is there.
 */
inline std::optional<ServingCell> parseXmonitor(std::string_view line) {
    auto fields = parseAtFields(line, "%XMONITOR:");
    if (!fields || fields->empty() || !(*fields)[0].asInt()) {
        return std::nullopt;
    }
    if (fields->size() < 12) {
        return ServingCell {};
    }
    const auto& f = *fields;
    auto string = [](const AtField& field) {
        return std::string(field.asString().value_or(""));
    };
    return ServingCell {
        .cellId = string(f[7]),
        .rat = fromAccessTechnology(f[5].asInt()),
        .plmn = string(f[3]),
        .operatorName = string(f[1]),
        .trackingAreaCode = string(f[4]),
        .band = f[6].asInt().value_or(0),
        .earfcn = f[9].asInt().value_or(0),
        .pci = f[8].asInt().value_or(0),
        .rsrp = detail::rsrpFromIndex(f[10].asInt()),
        .rsrq = std::nullopt,
        .sinr = detail::snrFromIndex(f[11].asInt()),
        .ecl = std::nullopt,
        .txPower = std::nullopt,
    };
}

/**
 * @brief Adds what %CONEVAL knows to the serving cell: RSRQ, coverage enhancement level and
 * transmit power. Only takes values for the same cell, and only from a successful evaluation.
 *
 * %CONEVAL: <result>,<rrc_state>,<energy_estimate>,<rsrp>,<rsrq>,<snr>,<cell_id>,<plmn>,
 * <phys_cell_id>,<earfcn>,<band>,<tau_triggered>,<ce_level>,<tx_power>,<tx_repetitions>,
 * <rx_repetitions>,<dl-pathloss>
 */
inline void addConeval(ServingCell& cell, std::string_view line) {
    auto fields = parseAtFields(line, "%CONEVAL:");
    if (!fields || fields->size() < 14 || (*fields)[0].asInt() != 0) {
        return;
    }
    const auto& f = *fields;
    if (f[6].asString().value_or("") != cell.cellId) {
        return;
    }
    cell.rsrq = detail::rsrqFromIndex(f[4].asInt());
    auto ceLevel = f[12].asInt();
    cell.ecl = ceLevel && *ceLevel >= 0 && *ceLevel <= 3 ? ceLevel : std::nullopt;
    auto txPower = f[13].asInt();
    cell.txPower = txPower && *txPower >= -50 && *txPower <= 30 ? txPower : std::nullopt;
}

/**
 * @brief Human-readable summary for the log.
 */
inline std::string describe(const ServingCell& cell) {
    if (!cell.isCamped()) {
        return "no serving cell";
    }
    std::string description = cell.rat ? toString(*cell.rat) : "unknown RAT";
    description += " on " + (cell.operatorName.empty() ? cell.plmn : cell.operatorName + " (" + cell.plmn + ")");
    description += ", band " + std::to_string(cell.band) + ", EARFCN " + std::to_string(cell.earfcn)
        + ", PCI " + std::to_string(cell.pci) + ", cell " + cell.cellId;
    auto append = [&](const char* name, std::optional<int> value, const char* unit) {
        if (value) {
            description += std::string(", ") + name + " " + std::to_string(*value) + unit;
        }
    };
    append("RSRP", cell.rsrp, " dBm");
    append("RSRQ", cell.rsrq, " dB");
    append("SINR", cell.sinr, " dB");
    append("ECL", cell.ecl, "");
    append("TX power", cell.txPower, " dBm");
    return description;
}

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
