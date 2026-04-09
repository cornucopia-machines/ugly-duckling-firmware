#pragma once

#include <chrono>
#include <optional>

#include <utils/Chrono.hpp>

#include "IScheduler.hpp"

using namespace std::chrono;
using namespace std::chrono_literals;

namespace cornucopia::ugly_duckling::utils::scheduling {

struct DelaySchedule {
    seconds open;
    seconds close;
};

/**
 * @brief Wraps a scheduler to delay state transitions.
 *
 * When the inner scheduler requests a state change, this wrapper waits
 * for the configured delay before actually committing to the new state.
 *
 * The delay scheduler maintains three key pieces of state:
 *
 * - committedState: The state currently being reported to the caller
 * - pendingState: The state the inner scheduler wants to transition to
 * - transitionStartTime: When the inner scheduler first requested the pending state
 *
 * When the inner scheduler requests a different state than committedState,
 * the delay scheduler starts a timer. If the inner scheduler continues to
 * request that same state for the full delay period, the scheduler commits
 * to the new state. If the inner scheduler changes its mind before the delay
 * elapses, the timer is reset.
 */
class DelayScheduler : public IScheduler {
public:
    DelayScheduler(const std::shared_ptr<IScheduler>& innerScheduler)
        : innerScheduler(innerScheduler) {
    }

    void setTarget(const DelaySchedule& target) {
        LOGTD(SCHEDULING, "DelayScheduler: Setting target: open=%lld s, close=%lld s",
            target.open.count(), target.close.count());
        this->target = target;
    }

    const char* getName() const override {
        return "delay";
    }

    ScheduleResult tick() override {
        return tick(steady_clock::now());
    }

    ScheduleResult tick(steady_clock::time_point now) {
        auto innerResult = innerScheduler->tick();
        auto desiredState = innerResult.targetState;

        // If inner scheduler has no opinion, maintain current state
        if (!desiredState) {
            resetTransition();
            return {
                .targetState = committedState,
                .nextDeadline = innerResult.nextDeadline,
                .shouldPublishTelemetry = innerResult.shouldPublishTelemetry,
            };
        }

        // If we haven't committed to a state yet, do so immediately
        if (!committedState) {
            LOGTD(SCHEDULING, "Initial commit to state %s", toString(desiredState));
            committedState = desiredState;
            resetTransition();
            return {
                .targetState = committedState,
                .nextDeadline = innerResult.nextDeadline,
                .shouldPublishTelemetry = true,
            };
        }

        // If desired state matches committed state, no transition needed
        if (desiredState == committedState) {
            resetTransition();
            return {
                .targetState = committedState,
                .nextDeadline = innerResult.nextDeadline,
                .shouldPublishTelemetry = innerResult.shouldPublishTelemetry,
            };
        }

        // Start a new transition
        if (!transitionStartTime || pendingState != desiredState) {
            LOGTD(SCHEDULING, "Starting transition from %s to %s",
                toString(committedState),
                toString(desiredState));
            transitionStartTime = now;
            pendingState = desiredState;
        }

        // Determine which delay to use
        auto delay = (*desiredState == TargetState::Open) ? target.open : target.close;
        auto elapsed = duration_cast<seconds>(now - *transitionStartTime);

        // Check if delay has elapsed
        if (elapsed >= delay) {
            LOGTD(SCHEDULING, "Committing to state %s after %lld s delay",
                toString(desiredState),
                elapsed.count());
            committedState = desiredState;
            resetTransition();
            return {
                .targetState = committedState,
                .nextDeadline = innerResult.nextDeadline,
                .shouldPublishTelemetry = true,
            };
        }

        // Still waiting for delay to elapse
        auto remaining = delay - elapsed;
        LOGTV(SCHEDULING, "Waiting %lld more seconds before transitioning to %s",
            duration_cast<seconds>(remaining).count(),
            toString(desiredState));

        return {
            .targetState = committedState,
            .nextDeadline = minDuration(innerResult.nextDeadline, duration_cast<ms>(remaining)),
            .shouldPublishTelemetry = false,
        };
    }

private:
    void resetTransition() {
        transitionStartTime.reset();
        pendingState.reset();
    }

    std::shared_ptr<IScheduler> innerScheduler;
    DelaySchedule target { .open = 0s, .close = 0s };

    std::optional<TargetState> committedState;
    std::optional<TargetState> pendingState;
    std::optional<time_point<steady_clock>> transitionStartTime;
};

}    // namespace cornucopia::ugly_duckling::utils::scheduling
