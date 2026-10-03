#pragma once

#include <drivers/cellular/AtResponse.hpp>

#include <charconv>
#include <chrono>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

/**
 * @brief Parses a modem timestamp, "YYYY/MM/DD,hh:mm:ss" or "YY/MM/DD,hh:mm:ss", with an optional
 * "±zz" time zone in quarters of an hour (the format of AT+CCLK).
 *
 * With a time zone, the timestamp is taken as local time in that zone, as 3GPP TS 27.007 defines
 * it for +CCLK, and converted to UTC; without one it is taken as UTC.
 */
inline std::optional<time_t> parseModemTimestamp(std::string_view text) {
    using namespace std::chrono;

    // strptime wants a NUL-terminated string
    std::string input(text);
    std::tm fields {};
    const char* rest = strptime(input.c_str(), "%Y/%m/%d,%H:%M:%S", &fields);
    if (rest == nullptr) {
        return std::nullopt;
    }
    int fullYear = fields.tm_year + 1900;
    if (fullYear < 100) {
        // %Y takes the two-digit form at face value, as the year 26
        fullYear += 2000;
    }
    year_month_day date { year(fullYear), month(static_cast<unsigned>(fields.tm_mon + 1)), day(static_cast<unsigned>(fields.tm_mday)) };
    if (!date.ok()) {
        return std::nullopt;
    }

    // Whatever strptime left over can only be the time zone
    minutes zoneOffset { 0 };
    const char* end = input.c_str() + input.size();
    if (rest != end) {
        char sign = *rest;
        int quarters = 0;
        auto [ptr, ec] = std::from_chars(rest + 1, end, quarters);
        if ((sign != '+' && sign != '-') || ec != std::errc {} || ptr != end || quarters > 96) {
            return std::nullopt;
        }
        zoneOffset = minutes((sign == '-' ? -quarters : quarters) * 15);
    }

    auto utc = sys_days(date) + hours(fields.tm_hour) + minutes(fields.tm_min) + seconds(fields.tm_sec) - zoneOffset;
    return static_cast<time_t>(duration_cast<seconds>(utc.time_since_epoch()).count());
}

/**
 * @brief Parses the network time the module reports with AT+CTZR=3:
 * +CTZEU: <tz>,<dst>[,<utime>], where <utime> is "YYYY/MM/DD,hh:mm:ss" in UTC.
 *
 * Returns nullopt for a time zone report without the time: the network doesn't have to send one.
 */
inline std::optional<time_t> parseCtzeu(std::string_view line) {
    auto fields = parseAtFields(line, "+CTZEU:");
    if (!fields || fields->size() < 3) {
        return std::nullopt;
    }
    auto utime = (*fields)[2].asString();
    if (!utime) {
        return std::nullopt;
    }
    return parseModemTimestamp(*utime);
}

/**
 * @brief Parses the "+QNTP: <result>,<time>" that follows AT+QNTP; nullopt unless the result is 0.
 */
inline std::optional<time_t> parseQntp(std::string_view line) {
    auto fields = parseAtFields(line, "+QNTP:");
    if (!fields || fields->size() < 2 || (*fields)[0].asInt() != 0) {
        return std::nullopt;
    }
    auto time = (*fields)[1].asString();
    if (!time) {
        return std::nullopt;
    }
    return parseModemTimestamp(*time);
}

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
