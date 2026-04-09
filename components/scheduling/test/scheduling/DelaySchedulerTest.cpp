#include <catch2/catch_test_macros.hpp>

#include "TestHelpers.hpp"

#include <chrono>
#include <memory>

#include <FakeLog.hpp>

#include <peripherals/api/TargetState.hpp>
#include <scheduling/DelayScheduler.hpp>
#include <scheduling/IScheduler.hpp>

using namespace std::chrono;
using namespace std::chrono_literals;
using namespace cornucopia::ugly_duckling::peripherals::api;
using namespace cornucopia::ugly_duckling::utils::scheduling;

namespace {

/**
 * @brief Mock scheduler for testing that returns a configurable state.
 */
class MockScheduler : public IScheduler {
public:
    void setTarget(std::optional<TargetState> state, std::optional<ms> deadline = std::nullopt) {
        this->state = state;
        this->nextDeadline = deadline;
    }

    const char* getName() const override {
        return "mock";
    }

    ScheduleResult tick() override {
        return {
            .targetState = state,
            .nextDeadline = nextDeadline,
            .shouldPublishTelemetry = false,
        };
    }

private:
    std::optional<TargetState> state;
    std::optional<ms> nextDeadline;
};

}    // namespace

static const auto T0 = steady_clock::time_point();

TEST_CASE("DelayScheduler: transitions immediately with zero delay") {
    auto mockScheduler = std::make_shared<MockScheduler>();
    DelayScheduler delayScheduler(mockScheduler);
    delayScheduler.setTarget({ .open = 0s, .close = 0s });

    mockScheduler->setTarget(TargetState::Open);
    REQUIRE(delayScheduler.tick(T0) == ScheduleResult { .targetState = TargetState::Open, .shouldPublishTelemetry = true });

    mockScheduler->setTarget(TargetState::Closed);
    REQUIRE(delayScheduler.tick(T0) == ScheduleResult { .targetState = TargetState::Closed, .shouldPublishTelemetry = true });

    mockScheduler->setTarget(TargetState::Open);
    REQUIRE(delayScheduler.tick(T0) == ScheduleResult { .targetState = TargetState::Open, .shouldPublishTelemetry = true });
}

TEST_CASE("DelayScheduler: commits immediately with no previous commitment") {
    auto mockScheduler = std::make_shared<MockScheduler>();
    DelayScheduler delayScheduler(mockScheduler);
    delayScheduler.setTarget({ .open = 10s, .close = 10s });

    // Start with open (commits immediately)
    mockScheduler->setTarget(TargetState::Open);
    REQUIRE(delayScheduler.tick(T0) == ScheduleResult { .targetState = TargetState::Open, .shouldPublishTelemetry = true });    // Commits immediately

    // Transition to closed (delayed)
    mockScheduler->setTarget(TargetState::Closed);
    REQUIRE(delayScheduler.tick(T0) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 10s });    // Still open, waiting for delay
}

TEST_CASE("DelayScheduler: returns none when inner scheduler has no opinion") {
    auto mockScheduler = std::make_shared<MockScheduler>();
    DelayScheduler delayScheduler(mockScheduler);
    delayScheduler.setTarget({ .open = 10s, .close = 10s });

    mockScheduler->setTarget(std::nullopt);
    REQUIRE(delayScheduler.tick(T0) == ScheduleResult { .targetState = std::nullopt });
}

TEST_CASE("DelayScheduler: returns committed state when inner scheduler has no opinion") {
    auto mockScheduler = std::make_shared<MockScheduler>();
    DelayScheduler delayScheduler(mockScheduler);
    delayScheduler.setTarget({ .open = 10s, .close = 10s });

    // Start with open
    mockScheduler->setTarget(TargetState::Open);
    REQUIRE(delayScheduler.tick(T0) == ScheduleResult { .targetState = TargetState::Open, .shouldPublishTelemetry = true });

    // Now inner scheduler has no opinion
    mockScheduler->setTarget(std::nullopt);
    REQUIRE(delayScheduler.tick(T0) == ScheduleResult { .targetState = TargetState::Open });
}

TEST_CASE("DelayScheduler: delays opening") {
    auto mockScheduler = std::make_shared<MockScheduler>();
    DelayScheduler delayScheduler(mockScheduler);
    delayScheduler.setTarget({ .open = 10s, .close = 5s });

    // Start with closed
    mockScheduler->setTarget(TargetState::Closed);
    REQUIRE(delayScheduler.tick(T0) == ScheduleResult { .targetState = TargetState::Closed, .shouldPublishTelemetry = true });

    // Request to open
    mockScheduler->setTarget(TargetState::Open);
    REQUIRE(delayScheduler.tick(T0) == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 10s });

    // Simulate time passing (5 seconds) - not enough
    REQUIRE(delayScheduler.tick(T0 + 5s) == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 5s });

    // After full delay, should commit to open
    REQUIRE(delayScheduler.tick(T0 + 10s) == ScheduleResult { .targetState = TargetState::Open, .shouldPublishTelemetry = true });

    // Further ticks should maintain open state
    REQUIRE(delayScheduler.tick(T0 + 11s) == ScheduleResult { .targetState = TargetState::Open });
}

TEST_CASE("DelayScheduler: delays closing") {
    auto mockScheduler = std::make_shared<MockScheduler>();
    DelayScheduler delayScheduler(mockScheduler);
    delayScheduler.setTarget({ .open = 5s, .close = 10s });

    // Start with open
    mockScheduler->setTarget(TargetState::Open);
    REQUIRE(delayScheduler.tick(T0) == ScheduleResult { .targetState = TargetState::Open, .shouldPublishTelemetry = true });

    // Request to close
    mockScheduler->setTarget(TargetState::Closed);
    REQUIRE(delayScheduler.tick(T0) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 10s });    // Stays open

    REQUIRE(delayScheduler.tick(T0 + 2s) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 8s });    // Still open

    // Wait for delay
    REQUIRE(delayScheduler.tick(T0 + 10s) == ScheduleResult { .targetState = TargetState::Closed, .shouldPublishTelemetry = true });
}

TEST_CASE("DelayScheduler: resets timer on state change") {
    auto mockScheduler = std::make_shared<MockScheduler>();
    DelayScheduler delayScheduler(mockScheduler);
    delayScheduler.setTarget({ .open = 5s, .close = 5s });

    // Start with closed
    mockScheduler->setTarget(TargetState::Closed);
    REQUIRE(delayScheduler.tick(T0) == ScheduleResult { .targetState = TargetState::Closed, .shouldPublishTelemetry = true });

    // Request open
    mockScheduler->setTarget(TargetState::Open);
    REQUIRE(delayScheduler.tick() == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 5s });    // Still closed

    // Change mind back to closed after 3 seconds - timer should reset
    mockScheduler->setTarget(TargetState::Closed);
    REQUIRE(delayScheduler.tick(T0 + 3s) == ScheduleResult { .targetState = TargetState::Closed });

    // Now request open again implies the same amount of delay again
    mockScheduler->setTarget(TargetState::Open);
    REQUIRE(delayScheduler.tick(T0 + 3s) == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 5s });    // Still closed
}

TEST_CASE("DelayScheduler: maintains committed state when inner scheduler returns to it") {
    auto mockScheduler = std::make_shared<MockScheduler>();
    DelayScheduler delayScheduler(mockScheduler);
    delayScheduler.setTarget({ .open = 5s, .close = 5s });

    // Start with open
    mockScheduler->setTarget(TargetState::Open);
    REQUIRE(delayScheduler.tick(T0) == ScheduleResult { .targetState = TargetState::Open, .shouldPublishTelemetry = true });

    // Request closed
    mockScheduler->setTarget(TargetState::Closed);
    REQUIRE(delayScheduler.tick(T0) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 5s });    // Still open

    // Wait a bit
    // Inner scheduler changes back to open
    mockScheduler->setTarget(TargetState::Open);
    REQUIRE(delayScheduler.tick(T0 + 2s) == ScheduleResult { .targetState = TargetState::Open });    // No change, so no telemetry
}

TEST_CASE("DelayScheduler: respects inner scheduler's nextDeadline") {
    auto mockScheduler = std::make_shared<MockScheduler>();
    DelayScheduler delayScheduler(mockScheduler);
    delayScheduler.setTarget({ .open = 100s, .close = 100s });

    mockScheduler->setTarget(TargetState::Open, 30s);

    // When not in transition, should pass through inner deadline
    REQUIRE(delayScheduler.tick(T0).nextDeadline == 30s);
}

TEST_CASE("DelayScheduler: uses minimum of inner deadline and remaining delay") {
    auto mockScheduler = std::make_shared<MockScheduler>();
    DelayScheduler delayScheduler(mockScheduler);
    delayScheduler.setTarget({ .open = 5s, .close = 5s });

    // Start with closed
    mockScheduler->setTarget(TargetState::Closed);
    delayScheduler.tick(T0);

    // Request open with long inner deadline
    mockScheduler->setTarget(TargetState::Open, 100s);

    auto result = delayScheduler.tick(T0);

    // Should use the delay time, not the inner deadline
    REQUIRE(result.nextDeadline.has_value());
    REQUIRE(result.nextDeadline.value() <= 5s);

    // Wait partial delay
    result = delayScheduler.tick(T0 + 3s);

    // Remaining time should be even shorter
    REQUIRE(result.nextDeadline.has_value());
    REQUIRE(result.nextDeadline.value() <= 3s);
}

TEST_CASE("DelayScheduler: different delays for open and close") {
    auto mockScheduler = std::make_shared<MockScheduler>();
    DelayScheduler delayScheduler(mockScheduler);
    delayScheduler.setTarget({ .open = 2s, .close = 8s });

    // Start closed
    mockScheduler->setTarget(TargetState::Closed);
    REQUIRE(delayScheduler.tick(T0) == ScheduleResult { .targetState = TargetState::Closed, .shouldPublishTelemetry = true });

    // Request open - should take 2s
    mockScheduler->setTarget(TargetState::Open);
    REQUIRE(delayScheduler.tick(T0) == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 2s });
    REQUIRE(delayScheduler.tick(T0 + 2s) == ScheduleResult { .targetState = TargetState::Open, .shouldPublishTelemetry = true });

    auto T1 = T0 + 10s;

    // Request close - should take 8s
    mockScheduler->setTarget(TargetState::Closed);
    REQUIRE(delayScheduler.tick(T1) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 8s });    // Stay open
    REQUIRE(delayScheduler.tick(T1 + 5s) == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 3s });    // Still open after 5s
    REQUIRE(delayScheduler.tick(T1 + 8s) == ScheduleResult { .targetState = TargetState::Closed, .shouldPublishTelemetry = true });    // Now closed
}
