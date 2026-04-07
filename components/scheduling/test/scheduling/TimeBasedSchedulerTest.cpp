#include <catch2/catch_test_macros.hpp>

#include "TestHelpers.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <time.h>

#include <peripherals/api/IValve.hpp>
#include <scheduling/TimeBasedScheduler.hpp>

using namespace std::chrono;
using namespace std::chrono_literals;
using namespace farmhub::utils::scheduling;

namespace Catch {

template <>
struct StringMaker<TimeBasedSchedule> {
    static std::string convert(TimeBasedSchedule const& s) {
        using namespace std::chrono;
        std::ostringstream oss;
        // Format start as ISO8601 UTC
        auto tt = system_clock::to_time_t(s.start);
        std::tm tmStruct {};
#if defined(_WIN32)
        gmtime_s(&tmStruct, &tt);
#else
        gmtime_r(&tt, &tmStruct);
#endif
        char timeBuf[32];
        if (std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%SZ", &tmStruct) == 0) {
            std::snprintf(timeBuf, sizeof(timeBuf), "%lld", static_cast<long long>(duration_cast<seconds>(s.start.time_since_epoch()).count()));
        }
        oss << "TimeBasedSchedule{start=" << timeBuf
            << ", period=" << s.period.count() << "s"
            << ", duration=" << s.duration.count() << "s}";
        return oss.str();
    }
};

}    // namespace Catch

static const auto T0 = system_clock::from_time_t(1000000);

TEST_CASE("not scheduled when empty") {
    ScheduleResult update = TimeBasedScheduler::getStateUpdate({}, T0);
    REQUIRE(update == ScheduleResult { .targetState = {}, .nextDeadline = {} });
}

TEST_CASE("keeps closed until schedule starts") {
    std::vector<TimeBasedSchedule> schedules {
        TimeBasedSchedule { .start = T0, .period = 1h, .duration = 15s },
    };
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 - 1s) == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 1s });
}

TEST_CASE("keeps open when schedule is started and in period") {
    std::vector<TimeBasedSchedule> schedules {
        TimeBasedSchedule { .start = T0, .period = 1h, .duration = 15s },
    };
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 15s });
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 1s) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 14s });
}

TEST_CASE("keeps closed when schedule is started and outside period") {
    std::vector<TimeBasedSchedule> schedules {
        TimeBasedSchedule { .start = T0, .period = 1h, .duration = 15s },
    };
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 15s) == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 1h - 15s });
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 16s) == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 1h - 16s });
}

TEST_CASE("when there are overlapping schedules keep closed until earliest opens") {
    // --OOOOOO--------------
    // ----OOOOOO------------
    std::vector<TimeBasedSchedule> schedules {
        TimeBasedSchedule { .start = T0 + 5min, .period = 1h, .duration = 15min },
        TimeBasedSchedule { .start = T0 + 10min, .period = 1h, .duration = 15min },
    };
    // Keep closed until first schedule starts
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0) == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 5min });
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 1s) == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 5min - 1s });
}

TEST_CASE("when there are overlapping schedules keep open until latest closes") {
    // --OOOOOO--------------
    // ----OOOOOO------------
    std::vector<TimeBasedSchedule> schedules {
        TimeBasedSchedule { .start = T0 + 5min, .period = 1h, .duration = 15min },
        TimeBasedSchedule { .start = T0 + 10min, .period = 1h, .duration = 15min },
    };
    // Open when first schedule starts, and keep open
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 5min) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 15min });
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 5min + 1s) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 15min - 1s });
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 10min) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 15min });
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 15min) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 10min });
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 25min - 1s) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 1s });

    // Close again after later schedule ends, and reopen when first schedule starts again
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 25min) == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 40min });
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 25min + 1s) == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 40min - 1s });
}

TEST_CASE("handles back-to-back schedules without gap") {
    // Two schedules that touch end-to-start: [0..10s) and [10s..20s)
    std::vector<TimeBasedSchedule> schedules {
        TimeBasedSchedule { .start = T0, .period = 30s, .duration = 10s },
        TimeBasedSchedule { .start = T0 + 10s, .period = 30s, .duration = 10s },
    };

    // At start => Open for 10s
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 10s });
    // Just before switch => still Open
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 10s - 1ms) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 1ms });
    // Exactly at boundary => next schedule keeps it Open for another 10s
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 10s) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 10s });
    // After second ends => Closed until next period
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 20s) == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 10s });
}

TEST_CASE("stays closed until first open, then reverts correctly") {
    std::vector<TimeBasedSchedule> schedules {
        TimeBasedSchedule { .start = T0 + 5s, .period = 60s, .duration = 2s },
    };

    // Before first start => None
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0) == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 5s });
    // During open => Open
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 5s + 500ms) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 1500ms });
    // After close => None until next period
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 7s) == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 58s });
}

TEST_CASE("non-overlapping sequences alternate open and closed as expected") {
    std::vector<TimeBasedSchedule> schedules {
        TimeBasedSchedule { .start = T0 + 0s, .period = 20s, .duration = 5s },
        TimeBasedSchedule { .start = T0 + 10s, .period = 20s, .duration = 5s },
    };

    // 0..5s Open, 5..10s Closed, 10..15s Open, 15..20s Closed
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 0s) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 5s });
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 5s) == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 5s });
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 10s) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 5s });
    REQUIRE(TimeBasedScheduler::getStateUpdate(schedules, T0 + 15s) == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 5s });
}
