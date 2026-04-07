#pragma once

#include <chrono>

#include <freertos/FreeRTOS.h>        // NOLINT(misc-header-include-cycle)
#include <freertos/event_groups.h>    // NOLINT(misc-header-include-cycle)

#include <Time.hpp>

using namespace std::chrono;

namespace farmhub::kernel {

// 0th bit reserved to indicate that a state has changed
static constexpr int STATE_CHANGE_BIT_MASK = (1 << 0);

class StateManager;

/**
 * @brief Represents an observable state. Tasks can check the current state, or wait for the state to be set.
 *
 * Note that state can also be cleared, but this class does not allow waiting for that.
 */
class State {
public:
    State(const std::string& name, EventGroupHandle_t eventGroup, EventBits_t eventBits)
        : name(name)
        , eventGroup(eventGroup)
        , eventBits(eventBits) {
    }

    State(const State& event)
        = default;

    /**
     * @brief Checks if the state is set.
     */
    bool isSet() const {
        return awaitSet(ticks::zero());
    }

    /**
     * @brief Waits for the state to be set, or until timeout is elapsed.
     *
     * @return Whether the state was set before the timeout elapsed.
     */
    bool awaitSet(const ticks timeout) const {
        return hasAllBits(xEventGroupWaitBits(eventGroup, eventBits, 0, 1, timeout.count()));
    }

    /**
     * @brief Waits indefinitely for the state to be set.
     */
    void awaitSet() const {
        while (!awaitSet(ticks::max())) { }
    }

protected:
    bool constexpr hasAllBits(const EventBits_t bits) const {
        return (bits & eventBits) == eventBits;
    }

    const std::string name;
    EventGroupHandle_t eventGroup;
    EventBits_t eventBits;

    friend StateManager;
};

class StateSource
    : public State {
public:
    StateSource(const std::string& name, EventGroupHandle_t eventGroup, EventBits_t eventBits)
        : State(name, eventGroup, eventBits) {
    }

    StateSource(const StateSource& eventSource)
        = default;

    bool set() const {
        return hasAllBits(setBits(eventBits | STATE_CHANGE_BIT_MASK));
    }

    bool IRAM_ATTR setFromISR() const;

    bool clear() const {
        bool cleared = !hasAllBits(xEventGroupClearBits(eventGroup, eventBits));
        setBits(STATE_CHANGE_BIT_MASK);
        return cleared;
    }

    bool IRAM_ATTR clearFromISR() const;

private:
    EventBits_t setBits(EventBits_t bits) const {
        return xEventGroupSetBits(eventGroup, bits);
    }

    EventBits_t setBitsFromISR(EventBits_t bits) const {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        auto result = xEventGroupSetBitsFromISR(eventGroup, bits, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        return result;
    }
};

}    // namespace farmhub::kernel
