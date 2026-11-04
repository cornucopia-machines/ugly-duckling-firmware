#pragma once

#include <drivers/cellular/AtResponse.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

/**
 * @brief Parses the network time (NITZ) from the %XTIME URC the modem sends with AT%XTIME=1:
 * %XTIME: "<local_time_zone>","<universal_time>","<daylight_saving>", where <universal_time> is
 * UTC as 7 semi-octets, each byte's digits swapped (3GPP TS 24.008, 10.5.3.9): "62113032748340"
 * is 2026-11-03 23:47:38. The seventh byte is the time zone, which UTC doesn't need.
 *
 * Returns nullopt when the URC comes without the time: the network doesn't have to send one.
 */
inline std::optional<time_t> parseXtime(std::string_view line) {
    using namespace std::chrono;

    auto fields = parseAtFields(line, "%XTIME:");
    if (!fields || fields->size() < 2) {
        return std::nullopt;
    }
    auto text = (*fields)[1].asString();
    if (!text || text->size() < 12) {
        return std::nullopt;
    }
    std::array<int, 6> values {};
    for (size_t i = 0; i < values.size(); i++) {
        // The low digit comes first
        char low = (*text)[i * 2];
        char high = (*text)[(i * 2) + 1];
        if (low < '0' || low > '9' || high < '0' || high > '9') {
            return std::nullopt;
        }
        values[i] = ((high - '0') * 10) + (low - '0');
    }
    auto [yy, mon, dd, hh, mm, ss] = values;
    year_month_day date { year(2000 + yy), month(static_cast<unsigned>(mon)), day(static_cast<unsigned>(dd)) };
    if (!date.ok() || hh > 23 || mm > 59 || ss > 60) {
        return std::nullopt;
    }
    auto utc = sys_days(date) + hours(hh) + minutes(mm) + seconds(ss);
    return static_cast<time_t>(duration_cast<seconds>(utc.time_since_epoch()).count());
}

namespace detail {

/**
 * @brief Reads "yy/MM/dd,hh:mm:ss" as a time, without a time zone.
 */
inline std::optional<std::chrono::sys_seconds> parseClockTime(std::string_view text) {
    using namespace std::chrono;

    static constexpr std::string_view SHAPE = "00/00/00,00:00:00";
    if (text.size() != SHAPE.size()) {
        return std::nullopt;
    }
    std::array<int, 6> values {};
    for (size_t i = 0; i < values.size(); i++) {
        char high = text[i * 3];
        char low = text[(i * 3) + 1];
        if (high < '0' || high > '9' || low < '0' || low > '9'
            || (i + 1 < values.size() && text[(i * 3) + 2] != SHAPE[(i * 3) + 2])) {
            return std::nullopt;
        }
        values[i] = ((high - '0') * 10) + (low - '0');
    }
    auto [yy, mon, dd, hh, mm, ss] = values;
    year_month_day date { year(2000 + yy), month(static_cast<unsigned>(mon)), day(static_cast<unsigned>(dd)) };
    if (!date.ok() || hh > 23 || mm > 59 || ss > 60) {
        return std::nullopt;
    }
    return sys_days(date) + hours(hh) + minutes(mm) + seconds(ss);
}

}    // namespace detail

/**
 * @brief Parses the modem's clock from the answer to AT+CCLK?: +CCLK: "yy/MM/dd,hh:mm:ss±zz", the
 * local time, with the time zone in quarters of an hour from UTC (−48 to +48). The modem sets it
 * from the network time (NITZ), or AT+CCLK sets it; until then, AT+CCLK? answers ERROR.
 *
 * @return the time in UTC, or nullopt if the line isn't one, or the time is malformed
 */
inline std::optional<time_t> parseCclk(std::string_view line) {
    using namespace std::chrono;

    auto fields = parseAtFields(line, "+CCLK:");
    if (!fields || fields->empty()) {
        return std::nullopt;
    }
    auto text = (*fields)[0].asString();
    static constexpr size_t TIME_LENGTH = 17;
    if (!text || text->size() < TIME_LENGTH + 2 || text->size() > TIME_LENGTH + 3) {
        return std::nullopt;
    }
    auto local = detail::parseClockTime(text->substr(0, TIME_LENGTH));
    char sign = (*text)[TIME_LENGTH];
    if (!local || (sign != '+' && sign != '-')) {
        return std::nullopt;
    }
    int quarters = 0;
    for (char digit : text->substr(TIME_LENGTH + 1)) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        quarters = (quarters * 10) + (digit - '0');
    }
    if (quarters > 48) {
        return std::nullopt;
    }
    auto offset = minutes(15 * (sign == '-' ? -quarters : quarters));
    return static_cast<time_t>((*local - offset).time_since_epoch().count());
}

/**
 * @brief Formats a UTC time for AT+CCLK=: "yy/MM/dd,hh:mm:ss+00".
 *
 * @return nullopt for a time outside 2000–2099, which the two-digit year can't hold
 */
inline std::optional<std::string> formatCclk(time_t utc) {
    using namespace std::chrono;

    sys_seconds time { seconds(utc) };
    auto midnight = floor<days>(time);
    year_month_day date { midnight };
    int yearValue = static_cast<int>(date.year());
    if (yearValue < 2000 || yearValue > 2099) {
        return std::nullopt;
    }
    hh_mm_ss clock { time - midnight };
    std::array<char, 24> buffer {};
    std::snprintf(buffer.data(), buffer.size(), "%02d/%02u/%02u,%02d:%02d:%02d+00",
        yearValue - 2000, static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
        static_cast<int>(clock.hours().count()), static_cast<int>(clock.minutes().count()),
        static_cast<int>(clock.seconds().count()));
    return std::string(buffer.data());
}

// An SNTP packet: 48 bytes, client and server alike (RFC 4330)
inline constexpr size_t SNTP_PACKET_SIZE = 48;

/**
 * @brief An SNTP client request: version 4, client mode, everything else zero.
 */
inline std::array<uint8_t, SNTP_PACKET_SIZE> makeSntpRequest() {
    std::array<uint8_t, SNTP_PACKET_SIZE> packet {};
    // LI 0, VN 4, mode 3 (client)
    packet[0] = 0x23;
    return packet;
}

/**
 * @brief The server's transmit time from an SNTP reply, in UTC.
 *
 * @return nullopt for anything but a server reply with a time: the wrong mode or size, or a
 * kiss-o'-death (stratum 0)
 */
inline std::optional<time_t> parseSntpResponse(const uint8_t* packet, size_t length) {
    if (length < SNTP_PACKET_SIZE) {
        return std::nullopt;
    }
    // Mode 4 (server); stratum 0 means kiss-o'-death, no time
    if ((packet[0] & 0x07) != 4 || packet[1] == 0) {
        return std::nullopt;
    }
    uint32_t seconds = (static_cast<uint32_t>(packet[40]) << 24) | (static_cast<uint32_t>(packet[41]) << 16)
        | (static_cast<uint32_t>(packet[42]) << 8) | static_cast<uint32_t>(packet[43]);
    if (seconds == 0) {
        return std::nullopt;
    }
    // NTP counts from 1900; era 1 starts in February 2036, so small values belong to it (RFC 4330,
    // section 3)
    static constexpr int64_t NTP_TO_UNIX = 2208988800LL;
    int64_t ntpSeconds = seconds;
    if (seconds < 0x80000000U) {
        ntpSeconds += 0x100000000LL;
    }
    return static_cast<time_t>(ntpSeconds - NTP_TO_UNIX);
}

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
