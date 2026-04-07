#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <Concurrent.hpp>
#include <Log.hpp>
#include <Task.hpp>
#include <Telemetry.hpp>
#include <Time.hpp>

#include <peripherals/api/TargetState.hpp>
#include <scheduling/IScheduler.hpp>

using namespace std::chrono_literals;
using namespace farmhub::kernel;
using namespace farmhub::peripherals::api;
using namespace farmhub::utils::scheduling;

namespace farmhub::functions {

/**
 * @brief Common run loop for scheduled transitions of peripherals.
 *
 * This template function encapsulates the common pattern used in PlotController and ChickenDoor
 * for managing scheduled state transitions of peripherals (valves, doors, etc).
 *
 * @tparam TPeripheral The peripheral type that must have a transitionTo(TargetState) method
 * @tparam TConfigSpec The configuration specification type
 * @param name The name of the function instance for logging
 * @param loggingTag The logging tag to use for log messages
 * @param peripheral The peripheral to control
 * @param scheduler The composite scheduler that determines target states
 * @param telemetryPublisher The telemetry publisher for requesting telemetry updates
 * @param configQueue The queue for receiving configuration updates
 * @param configHandler Lambda function to handle configuration updates, called with the config spec
 */
template <typename TPeripheral, typename TConfigSpec>
void runScheduledTransitionLoop(
    const std::string& name,
    const char* loggingTag,
    const std::shared_ptr<TPeripheral>& peripheral,
    const std::shared_ptr<IScheduler>& scheduler,
    const std::shared_ptr<TelemetryPublisher>& telemetryPublisher,
    Queue<TConfigSpec>& configQueue,
    std::function<void(const TConfigSpec&)> configHandler) {

    Task::run(name, 4096, [name, loggingTag, peripheral, scheduler, telemetryPublisher, &configQueue, configHandler](Task& _task) {
        auto shouldPublishTelemetry = true;
        while (true) {
            ScheduleResult result = scheduler->tick();
            shouldPublishTelemetry |= result.shouldPublishTelemetry;

            auto nextDeadline = clampTicks(result.nextDeadline.value_or(ms::max()));

            // Default to Closed when no value is decided
            auto targetState = result.targetState.value_or(TargetState::Closed);

            auto transitionHappened = peripheral->transitionTo(targetState);
            if (transitionHappened) {
                LOGTI(loggingTag, "Function '%s' transitioned to state %s, will re-evaluate every %lld s",
                    name.c_str(),
                    toString(targetState),
                    duration_cast<seconds>(nextDeadline).count());
            } else {
                LOGTD(loggingTag, "Function '%s' stayed in state %s, will evaluate again after %lld s",
                    name.c_str(),
                    toString(targetState),
                    duration_cast<seconds>(nextDeadline).count());
            }
            shouldPublishTelemetry |= transitionHappened;

            if (shouldPublishTelemetry) {
                telemetryPublisher->requestTelemetryPublishing();
                shouldPublishTelemetry = false;
            }

            // TODO Account for time spent in transitionTo()
            configQueue.pollIn(nextDeadline, [&](const TConfigSpec& config) {
                configHandler(config);
                shouldPublishTelemetry = true;
            });
        }
    });
}

}    // namespace farmhub::functions
