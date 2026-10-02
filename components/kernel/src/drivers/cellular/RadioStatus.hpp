#pragma once

#include <drivers/cellular/AtResponse.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

/**
 * @brief Signal strength from +CSQ: <rssi>,<ber> (3GPP TS 27.007).
 */
struct SignalQuality {
    // Received signal strength in dBm; nullopt when the module reports 99, "not known"
    std::optional<int> rssiDbm;
};

/**
 * @brief Parses "+CSQ: <rssi>,<ber>".
 *
 * <rssi> is a level, not dBm: 0 is -113 dBm or less, 1 is -111 dBm, 2-30 step 2 dB from
 * -109 to -53 dBm, 31 is -51 dBm or more, and 99 means not known or not detectable.
 */
inline std::optional<SignalQuality> parseCsq(std::string_view line) {
    auto fields = parseAtFields(line, "+CSQ:");
    if (!fields || fields->empty()) {
        return std::nullopt;
    }
    auto level = (*fields)[0].asInt();
    if (!level || *level < 0 || (*level > 31 && *level != 99)) {
        return std::nullopt;
    }
    if (*level == 99) {
        return SignalQuality { .rssiDbm = std::nullopt };
    }
    return SignalQuality { .rssiDbm = -113 + (2 * *level) };
}

/**
 * @brief Serving cell radio state, from the BC660K-GL's "+QENG: 0,..." (AT+QENG=0).
 */
struct ServingCell {
    int earfcn;
    int pci;
    std::string cellId;
    // dBm, dB, dBm, dB
    std::optional<int> rsrp;
    std::optional<int> rsrq;
    std::optional<int> rssi;
    std::optional<int> sinr;
    int band;
    std::string trackingAreaCode;
    // Coverage enhancement level 0-2; only known in RRC connected state
    std::optional<int> ecl;
    // dBm
    std::optional<int> txPower;
    // 0 in-band same PCI, 1 in-band different PCI, 2 guard band, 3 stand-alone
    std::optional<int> operationMode;

    /**
     * @brief Whether the module has a serving cell at all. While searching it reports
     * placeholders: EARFCN 0, cell ID "00000000", RSRP -157.
     */
    bool isCamped() const {
        return earfcn != 0;
    }
};

/**
 * @brief Parses the serving cell line of the AT+QENG=0 response.
 *
 * +QENG: 0,<sc_EARFCN>,<sc_EARFCN_offset>,<sc_pci>,<sc_cellID>,[<sc_RSRP>],[<sc_RSRQ>],
 * [<sc_RSSI>],[<sc_SINR>],<sc_band>,<sc_TAC>,[<sc_ECL>],[<sc_Tx_pwr>],<operation_mode>
 *
 * Neighbor cells come as separate "+QENG: 1,..." lines, which this ignores.
 */
inline std::optional<ServingCell> parseQengServingCell(std::string_view line) {
    auto fields = parseAtFields(line, "+QENG:");
    if (!fields || fields->size() < 14 || (*fields)[0].asInt() != 0) {
        return std::nullopt;
    }
    const auto& f = *fields;
    auto earfcn = f[1].asInt();
    auto pci = f[3].asInt();
    auto band = f[9].asInt();
    if (!earfcn || !pci || !band) {
        return std::nullopt;
    }
    auto inRange = [](std::optional<int> value, int min, int max) -> std::optional<int> {
        return value && *value >= min && *value <= max ? value : std::nullopt;
    };
    return ServingCell {
        .earfcn = *earfcn,
        .pci = *pci,
        .cellId = std::string(f[4].asString().value_or("")),
        .rsrp = f[5].asInt(),
        .rsrq = f[6].asInt(),
        .rssi = f[7].asInt(),
        .sinr = f[8].asInt(),
        .band = *band,
        .trackingAreaCode = std::string(f[10].asString().value_or("")),
        // Out-of-range values (255) mean "not known"
        .ecl = inRange(f[11].asInt(), 0, 2),
        // Range -45..23 dBm; 128 (or -128 while searching) means invalid
        .txPower = inRange(f[12].asInt(), -45, 23),
        .operationMode = inRange(f[13].asInt(), 0, 3),
    };
}

/**
 * @brief Human-readable summary for the log.
 */
inline std::string describe(const ServingCell& cell) {
    if (!cell.isCamped()) {
        return "no serving cell";
    }
    std::string description = "band " + std::to_string(cell.band) + ", EARFCN " + std::to_string(cell.earfcn)
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
    return description;
}

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
