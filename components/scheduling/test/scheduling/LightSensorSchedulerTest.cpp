#include <catch2/catch_test_macros.hpp>

#include "TestHelpers.hpp"

#include <chrono>
#include <memory>

#include <FakeLog.hpp>

#include <peripherals/api/ILightSensor.hpp>
#include <peripherals/api/TargetState.hpp>
#include <scheduling/LightSensorScheduler.hpp>

using namespace std::chrono;
using namespace std::chrono_literals;
using namespace cornucopia::ugly_duckling::peripherals::api;
using namespace cornucopia::ugly_duckling::utils::scheduling;

namespace {

/**
 * @brief Mock light sensor for testing that returns a configurable light level.
 */
class MockLightSensor : public ILightSensor {
public:
    MockLightSensor() = default;

    void setLightLevel(Lux level) {
        this->level = level;
    }

    Lux getLightLevel() override {
        return level;
    }

    const std::string& getName() const override {
        static const std::string name = "mock-light-sensor";
        return name;
    }

private:
    Lux level = 0.0;
};

}    // namespace

TEST_CASE("LightSensorScheduler: returns none when no target is set") {
    auto mockSensor = std::make_shared<MockLightSensor>();
    LightSensorScheduler scheduler(mockSensor);

    mockSensor->setLightLevel(100.0);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = std::nullopt });
}

TEST_CASE("LightSensorScheduler: opens when light level exceeds open threshold") {
    auto mockSensor = std::make_shared<MockLightSensor>();
    LightSensorScheduler scheduler(mockSensor);
    scheduler.setTarget(LightSensorSchedule { .open = 100.0, .close = 50.0 });

    mockSensor->setLightLevel(100.0);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 1min });

    mockSensor->setLightLevel(150.0);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 1min });
}

TEST_CASE("LightSensorScheduler: closes when light level falls below close threshold") {
    auto mockSensor = std::make_shared<MockLightSensor>();
    LightSensorScheduler scheduler(mockSensor);
    scheduler.setTarget(LightSensorSchedule { .open = 100.0, .close = 50.0 });

    mockSensor->setLightLevel(50.0);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 1min });

    mockSensor->setLightLevel(30.0);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 1min });
}

TEST_CASE("LightSensorScheduler: returns no opinion in hysteresis zone") {
    auto mockSensor = std::make_shared<MockLightSensor>();
    LightSensorScheduler scheduler(mockSensor);
    scheduler.setTarget(LightSensorSchedule { .open = 100.0, .close = 50.0 });

    // Light level between close and open thresholds - no opinion
    mockSensor->setLightLevel(75.0);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = std::nullopt, .nextDeadline = 1min });

    mockSensor->setLightLevel(51.0);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = std::nullopt, .nextDeadline = 1min });

    mockSensor->setLightLevel(99.9);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = std::nullopt, .nextDeadline = 1min });
}

TEST_CASE("LightSensorScheduler: handles exact threshold boundaries") {
    auto mockSensor = std::make_shared<MockLightSensor>();
    LightSensorScheduler scheduler(mockSensor);
    scheduler.setTarget(LightSensorSchedule { .open = 100.0, .close = 50.0 });

    // Exactly at open threshold - should open
    mockSensor->setLightLevel(100.0);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 1min });

    // Exactly at close threshold - should close
    mockSensor->setLightLevel(50.0);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 1min });

    // Just below open threshold - no opinion
    mockSensor->setLightLevel(99.99);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = std::nullopt, .nextDeadline = 1min });

    // Just above close threshold - no opinion
    mockSensor->setLightLevel(50.01);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = std::nullopt, .nextDeadline = 1min });
}

TEST_CASE("LightSensorScheduler: handles same open and close thresholds") {
    auto mockSensor = std::make_shared<MockLightSensor>();
    LightSensorScheduler scheduler(mockSensor);
    scheduler.setTarget(LightSensorSchedule { .open = 75.0, .close = 75.0 });

    // Above threshold - open
    mockSensor->setLightLevel(76.0);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 1min });

    // At threshold - both open and close conditions met
    mockSensor->setLightLevel(75.0);
    auto result = scheduler.tick();
    // When both conditions are met, open takes precedence (checked first)
    REQUIRE(result == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 1min });

    // Below threshold - closed
    mockSensor->setLightLevel(74.0);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 1min });
}

TEST_CASE("LightSensorScheduler: always returns 1 minute deadline when target is set") {
    auto mockSensor = std::make_shared<MockLightSensor>();
    LightSensorScheduler scheduler(mockSensor);
    scheduler.setTarget(LightSensorSchedule { .open = 100.0, .close = 50.0 });

    // All states with a target should return 1min deadline
    mockSensor->setLightLevel(150.0);
    REQUIRE(scheduler.tick().nextDeadline == 1min);

    mockSensor->setLightLevel(75.0);
    REQUIRE(scheduler.tick().nextDeadline == 1min);

    mockSensor->setLightLevel(30.0);
    REQUIRE(scheduler.tick().nextDeadline == 1min);
}

TEST_CASE("LightSensorScheduler: changing target updates behavior") {
    auto mockSensor = std::make_shared<MockLightSensor>();
    LightSensorScheduler scheduler(mockSensor);
    mockSensor->setLightLevel(60.0);

    // First target configuration
    scheduler.setTarget(LightSensorSchedule { .open = 100.0, .close = 50.0 });
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = std::nullopt, .nextDeadline = 1min });

    // Change target configuration - now in "open" range
    scheduler.setTarget(LightSensorSchedule { .open = 50.0, .close = 30.0 });
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 1min });

    // Remove target
    scheduler.setTarget(std::nullopt);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = std::nullopt });
}

TEST_CASE("LightSensorScheduler: handles very low light levels") {
    auto mockSensor = std::make_shared<MockLightSensor>();
    LightSensorScheduler scheduler(mockSensor);
    scheduler.setTarget(LightSensorSchedule { .open = 10.0, .close = 1.0 });

    mockSensor->setLightLevel(0.0);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 1min });

    mockSensor->setLightLevel(0.5);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 1min });

    mockSensor->setLightLevel(5.0);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = std::nullopt, .nextDeadline = 1min });

    mockSensor->setLightLevel(15.0);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 1min });
}

TEST_CASE("LightSensorScheduler: handles zero as low light level") {
    auto mockSensor = std::make_shared<MockLightSensor>();
    LightSensorScheduler scheduler(mockSensor);
    scheduler.setTarget(LightSensorSchedule { .open = 10.0, .close = 0 });

    mockSensor->setLightLevel(0.0);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = TargetState::Closed, .nextDeadline = 1min });

    mockSensor->setLightLevel(0.5);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = std::nullopt, .nextDeadline = 1min });
}

TEST_CASE("LightSensorScheduler: handles very high light levels") {
    auto mockSensor = std::make_shared<MockLightSensor>();
    LightSensorScheduler scheduler(mockSensor);
    scheduler.setTarget(LightSensorSchedule { .open = 1000.0, .close = 500.0 });

    mockSensor->setLightLevel(2000.0);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 1min });

    mockSensor->setLightLevel(10000.0);
    REQUIRE(scheduler.tick() == ScheduleResult { .targetState = TargetState::Open, .nextDeadline = 1min });
}
