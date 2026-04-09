#pragma once

#include <chrono>
#include <optional>

#include <utils/Chrono.hpp>

#include <peripherals/api/ILightSensor.hpp>

#include "IScheduler.hpp"

using namespace std::chrono;
using namespace std::chrono_literals;

using namespace cornucopia::ugly_duckling::peripherals::api;

namespace cornucopia::ugly_duckling::utils::scheduling {

struct LightSensorSchedule {
    Lux open;
    Lux close;
};

struct LightSensorScheduler : IScheduler {
    LightSensorScheduler(const std::shared_ptr<ILightSensor>& lightSensor)
        : lightSensor(lightSensor) {
    }

    void setTarget(std::optional<LightSensorSchedule> target) {
        if (target) {
            LOGTD(SCHEDULING, "LightSensorScheduler: Setting target: open=%.2f lux, close=%.2f lux",
                target->open, target->close);
        } else {
            LOGTD(SCHEDULING, "LightSensorScheduler: Clearing target");
        }
        this->target = target;
    }

    const char* getName() const override {
        return "light";
    }

    ScheduleResult tick() override {
        if (!target) {
            return {
                .targetState = {},
                .nextDeadline = {},
                .shouldPublishTelemetry = false,
            };
        }
        const auto& target = *this->target;

        auto targetState = calculateTargetState(lightSensor->getLightLevel(), target);
        return {
            .targetState = targetState,
            .nextDeadline = 1min,
            .shouldPublishTelemetry = false,
        };
    }

private:
    static std::optional<TargetState> calculateTargetState(Lux lightLevel, const LightSensorSchedule& target) {
        if (lightLevel >= target.open) {
            return TargetState::Open;
        }
        if (lightLevel <= target.close) {
            return TargetState::Closed;
        }
        return {};
    }

    std::shared_ptr<ILightSensor> lightSensor;
    std::optional<LightSensorSchedule> target;
};

}    // namespace cornucopia::ugly_duckling::utils::scheduling
