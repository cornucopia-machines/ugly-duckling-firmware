#pragma once

#include <PwmManager.hpp>

#include <driver/gptimer.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>    // NOLINT(misc-header-include-cycle)
#include <freertos/semphr.h>      // NOLINT(misc-header-include-cycle)

#include <chrono>
#include <functional>

namespace cornucopia::ugly_duckling::kernel::drivers {

/**
 * @brief Ends a pulse on a pair of PWM channels from a hardware timer interrupt.
 *
 * @details `Task::delay()` only guarantees a minimum: the task can be kept from running by other
 * tasks, and on these chips every flash erase/write stalls all code running from flash for its
 * whole duration (tens of ms for an NVS commit). Neither affects this timer: the pulse is started
 * in the same critical section that starts the timer, and the alarm interrupt, its callback and
 * everything it calls run from IRAM (see `CONFIG_GPTIMER_ISR_CACHE_SAFE` and
 * `CONFIG_LEDC_CTRL_FUNC_IN_IRAM`).
 *
 * On the alarm, both channels are stopped with idle level 1, i.e. held statically high. For a
 * DRV88xx-style bridge input pair that is the brake state. The next `PwmPin::write()` on a channel
 * turns its PWM output back on. *
 * The GPTimer is allocated for the lifetime of the object; there are only a few of them (two on the
 * ESP32-C6), so create one for each pulse rather than keeping it around.
 */
class PulseTimer {
public:
    PulseTimer(const PwmPin& channelA, const PwmPin& channelB);
    ~PulseTimer();

    PulseTimer(const PulseTimer&) = delete;
    PulseTimer& operator=(const PulseTimer&) = delete;
    PulseTimer(PulseTimer&&) = delete;
    PulseTimer& operator=(PulseTimer&&) = delete;

    /**
     * @brief Run `start`, and hold both channels high `duration` after it.
     *
     * @details `start` runs in a critical section together with starting the timer, so nothing can
     * delay the timer relative to the start of the pulse. It must be short, and must not block or throw.
     * Blocks until the alarm has fired.
     *
     * @return false if the alarm did not fire in time; the channels are then held high from the task instead.
     */
    bool run(std::chrono::microseconds duration, const std::function<void()>& start);

private:
    static bool onAlarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t* event, void* context);

    void holdChannelsHigh() const;

    /**
     * @brief Stop and disable the timer, as far as it was started and enabled.
     */
    void release() noexcept;

    // Accessed from the ISR: the object must live in internal RAM (checked by gptimer_register_event_callbacks)
    const ledc_mode_t speedMode;
    const ledc_channel_t channelA;
    const ledc_channel_t channelB;
    gptimer_handle_t timer = nullptr;
    SemaphoreHandle_t done = nullptr;
    bool enabled = false;
    bool started = false;
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
