#pragma once

#include <drivers/cellular/AtResponse.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

/**
 * @brief EPS registration status, the <stat> of +CEREG (3GPP TS 27.007).
 */
enum class RegistrationStatus : uint8_t {
    NotSearching = 0,
    RegisteredHome = 1,
    Searching = 2,
    Denied = 3,
    Unknown = 4,
    RegisteredRoaming = 5,
};

inline const char* toString(RegistrationStatus status) {
    switch (status) {
        case RegistrationStatus::NotSearching:
            return "not registered, not searching";
        case RegistrationStatus::RegisteredHome:
            return "registered (home)";
        case RegistrationStatus::Searching:
            return "searching";
        case RegistrationStatus::Denied:
            return "registration denied";
        case RegistrationStatus::Unknown:
            return "unknown (out of coverage?)";
        case RegistrationStatus::RegisteredRoaming:
            return "registered (roaming)";
    }
    return "?";
}

/**
 * @brief Registration state as reported by +CEREG, with the location and reject cause
 * the module includes in URC mode 3.
 */
struct Registration {
    RegistrationStatus status;
    // Tracking area code and E-UTRAN cell ID, hex strings as the module reports them
    std::optional<std::string> trackingAreaCode;
    std::optional<std::string> cellId;
    // 7 = E-UTRAN, 9 = E-UTRAN (NB-S1 mode)
    std::optional<int> accessTechnology;
    // 0 = EMM cause (3GPP TS 24.008 Annex G), 1 = manufacturer-specific
    std::optional<int> causeType;
    // Why registration failed, e.g. EMM cause 15, "no suitable cells in tracking area"
    std::optional<int> rejectCause;

    bool isRegistered() const {
        return status == RegistrationStatus::RegisteredHome || status == RegistrationStatus::RegisteredRoaming;
    }
};

namespace detail {

// Fields from <stat> onward: <stat>[,[<tac>],[<ci>],[<AcT>][,<cause_type>,<reject_cause>]]
inline std::optional<Registration> parseRegistrationFields(const std::vector<AtField>& fields, size_t first) {
    if (fields.size() <= first) {
        return std::nullopt;
    }
    auto stat = fields[first].asInt();
    if (!stat || *stat < 0 || *stat > static_cast<int>(RegistrationStatus::RegisteredRoaming)) {
        return std::nullopt;
    }
    auto field = [&](size_t offset) -> AtField {
        return first + offset < fields.size() ? fields[first + offset] : AtField { {}, false };
    };
    auto string = [](const AtField& value) -> std::optional<std::string> {
        auto text = value.asString();
        return text ? std::optional<std::string>(*text) : std::nullopt;
    };
    return Registration {
        .status = static_cast<RegistrationStatus>(*stat),
        .trackingAreaCode = string(field(1)),
        .cellId = string(field(2)),
        .accessTechnology = field(3).asInt(),
        .causeType = field(4).asInt(),
        .rejectCause = field(5).asInt(),
    };
}

}    // namespace detail

/**
 * @brief Parses the unsolicited +CEREG: <stat>[,...] the module sends when registration changes.
 *
 * Not to be confused with the read response, which has <n> in front: "+CEREG: 3,2" is a read
 * response meaning "URC mode 3, searching", while as a URC it would read "denied, TAC 2".
 */
inline std::optional<Registration> parseCeregUrc(std::string_view line) {
    auto fields = parseAtFields(line, "+CEREG:");
    if (!fields) {
        return std::nullopt;
    }
    return detail::parseRegistrationFields(*fields, 0);
}

/**
 * @brief Parses the response to AT+CEREG?: +CEREG: <n>,<stat>[,...].
 */
inline std::optional<Registration> parseCeregRead(std::string_view line) {
    auto fields = parseAtFields(line, "+CEREG:");
    if (!fields || fields->size() < 2 || !(*fields)[0].asInt()) {
        return std::nullopt;
    }
    return detail::parseRegistrationFields(*fields, 1);
}

/**
 * @brief Human-readable summary for the log, e.g. "registration denied, EMM cause 15".
 */
inline std::string describe(const Registration& registration) {
    std::string description = toString(registration.status);
    if (registration.trackingAreaCode) {
        description += ", TAC " + *registration.trackingAreaCode;
    }
    if (registration.cellId) {
        description += ", cell " + *registration.cellId;
    }
    if (registration.rejectCause) {
        description += registration.causeType == 1 ? ", manufacturer cause " : ", EMM cause ";
        description += std::to_string(*registration.rejectCause);
    }
    return description;
}

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
