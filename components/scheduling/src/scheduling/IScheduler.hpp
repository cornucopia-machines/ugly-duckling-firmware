#pragma once

#include <chrono>
#include <optional>
#include <time.h>

#include <ArduinoJson.h>

#include <peripherals/api/TargetState.hpp>

namespace cornucopia::ugly_duckling::utils::scheduling {

LOGGING_TAG(SCHEDULING, "scheduling")

using ms = std::chrono::milliseconds;

struct ScheduleResult {
    // The state the scheduler decided to go for at this time, if any
    std::optional<TargetState> targetState;
    // Earliest time the scheduler needs to be called again (relative), or nullopt if ALAP
    std::optional<ms> nextDeadline;
    // Whether the caller should publish telemetry
    bool shouldPublishTelemetry { false };

    bool operator==(const ScheduleResult& other) const {
        return targetState == other.targetState
            && nextDeadline == other.nextDeadline
            && shouldPublishTelemetry == other.shouldPublishTelemetry;
    }
};

struct IScheduler {
    virtual ~IScheduler() = default;
    virtual ScheduleResult tick() = 0;
    virtual const char* getName() const = 0;
};

}    // namespace cornucopia::ugly_duckling::utils::scheduling

namespace ArduinoJson {

using namespace std::chrono;

template <>
struct Converter<system_clock::time_point> {
    static void toJson(const system_clock::time_point src, JsonVariant dst) {
        if (src == system_clock::time_point {}) {
            dst.set(nullptr);
            return;
        }
        time_t t = system_clock::to_time_t(src);
        tm tm {};
#ifdef _WIN32
        (void) localtime_s(&tm, &t);
#else
        (void) localtime_r(&t, &tm);
#endif
        char buf[64];
        (void) strftime(buf, sizeof(buf), "%FT%TZ", &tm);
        dst.set(buf);
    }

    static system_clock::time_point fromJson(JsonVariantConst src) {
        if (src.isNull()) {
            return system_clock::time_point {};
        }
        tm tm {};
#ifdef _WIN32
        // Windows doesn't have strptime, use sscanf to parse ISO 8601
        int year, month, day, hour, min, sec;
        if (sscanf(src.as<const char*>(), "%d-%d-%dT%d:%d:%dZ",
                   &year, &month, &day, &hour, &min, &sec) == 6) {
            tm.tm_year = year - 1900;
            tm.tm_mon = month - 1;
            tm.tm_mday = day;
            tm.tm_hour = hour;
            tm.tm_min = min;
            tm.tm_sec = sec;
            tm.tm_isdst = 0;
        }
#else
        strptime(src.as<const char*>(), "%FT%TZ", &tm);
        tm.tm_isdst = 0;
#endif
        return system_clock::from_time_t(mktime(&tm));
    }

    static bool checkJson(JsonVariantConst src) {
        return src.is<const char*>();
    }
};

}    // namespace ArduinoJson
