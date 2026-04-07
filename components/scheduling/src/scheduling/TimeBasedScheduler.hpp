#pragma once

#include <algorithm>
#include <chrono>
#include <optional>
#include <vector>

#include <scheduling/IScheduler.hpp>
#include <utils/Chrono.hpp>

namespace farmhub::utils::scheduling {

using namespace std::chrono;

struct TimeBasedSchedule {
    time_point<system_clock> start;
    seconds period;
    seconds duration;
};

class TimeBasedScheduler : public IScheduler {
public:
    const char* getName() const override {
        return "time";
    }

    void setSchedules(const std::vector<TimeBasedSchedule>& newSchedules) {
        LOGTD(SCHEDULING, "TimeBasedScheduler: Setting %zu schedules",
            newSchedules.size());
        schedules = newSchedules;
    }

    /**
     * @brief Determines the current valve state, and the next transition time based on given schedules and the current time.
     *
     * This function examines a list of valve schedules and the current time to decide the next state of the valve
     * and when the transition should occur. It accounts for overlapping schedules, preferring to keep the valve open
     * if any schedule demands it, and calculates the earliest necessary transition.
     *
     * @param schedules The list of TimeBasedSchedule objects representing the valve's operating schedule.
     * @param now The current time_point.
     * @return A structure indicating the current target state of the valve, and the time after which the next transition should occur.
     */
    static ScheduleResult getStateUpdate(const std::vector<TimeBasedSchedule>& schedules, time_point<system_clock> now) {
        auto targetState = std::optional<TargetState>();
        auto validFor = std::optional<milliseconds>();

        for (const auto& schedule : schedules) {
            auto offset = duration_cast<milliseconds>(now - schedule.start);
            LOGTV(SCHEDULING, "Offset from schedule start is %lld ms, schedule: open %lld sec / %lld sec",
                offset.count(),
                schedule.duration.count(),
                schedule.period.count());

            if (offset < 0ms) {
                // Schedule has not started yet; valve should be closed according to this schedule
                // Calculate when this schedule will start for the first time
                if (targetState != TargetState::Open) {
                    LOGTV(SCHEDULING, "Before schedule starts, should be closed");
                    targetState = TargetState::Closed;
                    validFor = minDuration(validFor, -offset);
                } else {
                    LOGTV(SCHEDULING, "Before schedule starts, but already open");
                }
            } else {
                // This schedule has started; determine if the valve should be open or closed according to this schedule
                auto periodPosition = offset % schedule.period;
                LOGTV(SCHEDULING, "Inside schedule at position %lld ms",
                    periodPosition.count());

                if (periodPosition < schedule.duration) {
                    // The valve should be open according to this schedule
                    // Calculate when this opening period will end
                    auto closeAfter = schedule.duration - periodPosition;
                    if (targetState == TargetState::Open) {
                        // We already found a schedule to keep this valve open, extend the period if possible
                        validFor = maxDuration(validFor, closeAfter);
                    } else {
                        // This is the first schedule to keep the valve open
                        targetState = TargetState::Open;
                        validFor = closeAfter;
                    }
                } else {
                    // The valve should be closed according to this schedule
                    if (targetState != TargetState::Open) {
                        // There are no other schedules to keep the valve open yet,
                        // calculate when the next opening period will start
                        targetState = TargetState::Closed;
                        auto openAfter = schedule.period - periodPosition;
                        validFor = minDuration(validFor, openAfter);
                    }
                }
            }
        }

        return {
            .targetState = targetState,
            .nextDeadline = validFor,
            .shouldPublishTelemetry = false,
        };
    }

    ScheduleResult tick() override {
        return getStateUpdate(schedules, std::chrono::system_clock::now());
    }

private:
    std::vector<TimeBasedSchedule> schedules;
};

}    // namespace farmhub::utils::scheduling

namespace ArduinoJson {

using farmhub::utils::scheduling::TimeBasedSchedule;

template <>
struct Converter<TimeBasedSchedule> {
    static void toJson(const TimeBasedSchedule& src, JsonVariant dst) {
        JsonObject obj = dst.to<JsonObject>();
        obj["start"] = src.start;
        obj["period"] = src.period.count();
        obj["duration"] = src.duration.count();
    }

    static TimeBasedSchedule fromJson(JsonVariantConst src) {
        auto start = src["start"].as<time_point<system_clock>>();
        auto period = seconds(src["period"].as<int64_t>());
        auto duration = seconds(src["duration"].as<int64_t>());
        return { .start = start, .period = period, .duration = duration };
    }

    static bool checkJson(JsonVariantConst src) {
        return src["start"].is<time_point<system_clock>>()
            && src["period"].is<int64_t>()
            && src["duration"].is<int64_t>();
    }
};

}    // namespace ArduinoJson
