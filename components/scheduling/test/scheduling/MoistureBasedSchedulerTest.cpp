#include <functional>

#include <catch2/catch_test_macros.hpp>

#include <FakeLog.hpp>

#include <scheduling/MoistureBasedScheduler.hpp>

#include "Fakes.hpp"

#include <utils/Chrono.hpp>

namespace cornucopia::ugly_duckling::utils::scheduling {

static constexpr SoilSimulator::Config BASIC_SOIL = {
    .gainPercentPerLiter = 1.0,
    .deadTime = 20s,
    .tau = 40s,
    .evaporationPercentPerMin = 0.05,
};

using SchedulerRef = MoistureBasedScheduler<FakeClock> const&;

struct SimulationConfig {
    seconds timeout = 30min;
    seconds defaultTick = 5s;

    Percent startMoisture = 55.0;
    Liters flowRatePerMinute = 15.0;
};

struct SimulationResult {
    ms time;
    int steps;
    Percent moisture;
    std::optional<TargetState> targetState;

    bool operator==(const SimulationResult& other) const {
        return time == other.time
            && steps == other.steps
            && moisture == other.moisture
            && targetState == other.targetState;
    }
};

struct Simulator {
    Simulator(SoilSimulator::Config soilConfig, MoistureTarget target, SimulationConfig simulationConfig)
        : Simulator(soilConfig, {}, target, simulationConfig) {
    }

    Simulator(SoilSimulator::Config soilConfig, MoistureBasedSchedulerSettings settings, std::optional<MoistureTarget> target, SimulationConfig simulationConfig)
        : clock(std::make_shared<FakeClock>())
        , flowMeter(std::make_shared<FakeFlowMeter>())
        , moistureSensor(std::make_shared<FakeSoilMoistureSensor>())
        , scheduler(settings, clock, flowMeter, moistureSensor)
        , simulationConfig(simulationConfig)
        , soil(soilConfig) {
        scheduler.setTarget(target);
        moistureSensor->moisture = simulationConfig.startMoisture;
    }

    SimulationResult runUntil(std::function<bool(SchedulerRef)> stopCondition) {
        ScheduleResult result;
        int steps = 0;
        while (clock->now() < simulationConfig.timeout) {
            result = scheduler.tick();
            steps++;
            auto tick = result.nextDeadline.value_or(simulationConfig.defaultTick);
            LOGTV(TEST, "At %lld sec in %s state, valve is %s, moisture level is %f%%, advancing by %lld sec",
                duration_cast<seconds>(clock->now()).count(),
                toString(scheduler.getState()),
                toString(result.targetState),
                scheduler.getTelemetry().moisture,
                duration_cast<seconds>(tick).count());

            if (stopCondition(scheduler)) {
                break;
            }

            // Produce flow when valve is on
            if (result.targetState == TargetState::Open) {
                const Liters volumePerTick = simulationConfig.flowRatePerMinute * chrono_ratio(tick, 1min);
                LOGTV(TEST, "Injecting %f liters of water", volumePerTick);
                flowMeter->bucket += volumePerTick;
                soil.inject(clock->now(), volumePerTick);
            }

            soil.step(clock->now(), moistureSensor->moisture, tick);

            clock->advance(tick);
        }

        LOGTV(TEST, "Final moisture level: %f after %lld sec, %d steps",
            scheduler.getTelemetry().moisture,
            duration_cast<seconds>(clock->now()).count(),
            steps);

        return {
            .time = clock->now(),
            .steps = steps,
            .moisture = scheduler.getTelemetry().moisture,
            .targetState = result.targetState,
        };
    }

    SimulationResult runUntilIdle() {
        return runUntil([](SchedulerRef scheduler) {
            return scheduler.getState() == State::Idle;
        });
    }

    std::shared_ptr<FakeClock> clock;
    std::shared_ptr<FakeFlowMeter> flowMeter;
    std::shared_ptr<FakeSoilMoistureSensor> moistureSensor;
    MoistureBasedScheduler<FakeClock> scheduler;
    SimulationConfig simulationConfig;
    SoilSimulator soil;
};

TEST_CASE("does not water when there is no target specified") {
    Simulator simulator(
        BASIC_SOIL,
        {},
        {},
        {
            .startMoisture = 10.0,
        });

    auto result = simulator.runUntilIdle();

    REQUIRE(result.time == 0ms);
    REQUIRE(result.steps == 1);
    REQUIRE(result.targetState.has_value() == false);
}

TEST_CASE("does not water when moisture is already above target") {
    Simulator simulator(
        BASIC_SOIL,
        {
            .low = 60,
            .high = 70,
        },
        {
            .startMoisture = 65.0,
        });

    auto result = simulator.runUntilIdle();

    REQUIRE(result == SimulationResult {
                .time = 0ms,
                .steps = 1,
                .moisture = 65.0,
                .targetState = TargetState::Closed,
            });
}

TEST_CASE("waters up to band without overshoot") {
    Simulator simulator(
        BASIC_SOIL,
        {
            .low = 60,
            .high = 70,
        },
        {
            .startMoisture = 55.0,
            .flowRatePerMinute = 15.0,
        });

    auto result = simulator.runUntilIdle();

    REQUIRE(result.targetState == TargetState::Closed);
    REQUIRE(result.time < 15min);
    REQUIRE(result.steps < 80);
    REQUIRE(result.moisture >= 60.0);
    REQUIRE(result.moisture <= 70.0);
}

TEST_CASE("starts watering after evaporation reduces moisture") {
    Simulator simulator(
        BASIC_SOIL,
        {
            .low = 60,
            .high = 70,
        },
        {
            .startMoisture = 61.0,
            .flowRatePerMinute = 15.0,
        });

    auto result = simulator.runUntil([](SchedulerRef scheduler) {
        return scheduler.getState() != State::Idle;
    });

    REQUIRE(result.targetState == TargetState::Open);
    REQUIRE(result.steps > 10);
    REQUIRE(result.moisture < 60.0);
    REQUIRE(result.moisture > 59.0);
}

}    // namespace cornucopia::ugly_duckling::utils::scheduling

namespace Catch {

using cornucopia::ugly_duckling::utils::scheduling::SimulationResult;

template <>
struct StringMaker<SimulationResult> {
    static std::string convert(SimulationResult const& s) {
        std::ostringstream oss;
        oss << "SimulationResult{time=" << s.time.count()
            << "ms, steps=" << s.steps
            << ", moisture=" << s.moisture
            << "%, targetState=" << toString(s.targetState) << "}";
        return oss.str();
    }
};

}    // namespace Catch
