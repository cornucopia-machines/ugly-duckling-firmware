#include "PulseTimer.hpp"

#include <EspException.hpp>
#include <Log.hpp>

#include <esp_attr.h>

#include <stdexcept>

namespace cornucopia::ugly_duckling::kernel::drivers {

// 1 tick = 1 µs
static constexpr uint32_t RESOLUTION_HZ = 1'000'000;

// How much longer than the pulse to wait for the alarm before giving up on it
static constexpr std::chrono::milliseconds ALARM_MARGIN { 100 };

PulseTimer::PulseTimer(const PwmPin& channelA, const PwmPin& channelB)
    : speedMode(channelA.getSpeedMode())
    , channelA(channelA.getChannel())
    , channelB(channelB.getChannel()) {
    if (channelB.getSpeedMode() != speedMode) {
        throw std::runtime_error("Pulse timer channels must use the same LEDC speed mode");
    }

    done = xSemaphoreCreateBinary();
    if (done == nullptr) {
        throw std::runtime_error("Could not create pulse timer semaphore");
    }

    gptimer_config_t config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = RESOLUTION_HZ,
        .intr_priority = 0,
        .flags = {},
    };
    gptimer_event_callbacks_t callbacks = {
        .on_alarm = onAlarm,
    };
    esp_err_t err = gptimer_new_timer(&config, &timer);
    if (err == ESP_OK) {
        err = gptimer_register_event_callbacks(timer, &callbacks, this);
    }
    if (err != ESP_OK) {
        // The destructor doesn't run when the constructor throws
        if (timer != nullptr) {
            gptimer_del_timer(timer);
        }
        vSemaphoreDelete(done);
        ESP_ERROR_THROW(err);
    }
}

PulseTimer::~PulseTimer() {
    if (timer != nullptr) {
        // In case run() threw halfway: the timer can only be deleted once stopped and disabled
        release();
        gptimer_del_timer(timer);
    }
    if (done != nullptr) {
        vSemaphoreDelete(done);
    }
}

bool PulseTimer::run(std::chrono::microseconds duration, const std::function<void()>& start) {
    gptimer_alarm_config_t alarm = {
        .alarm_count = static_cast<uint64_t>(duration.count()),
        .reload_count = 0,
        .flags = {
            .auto_reload_on_alarm = false,
        },
    };
    // Enabling the timer holds a power management lock until it's disabled again
    ESP_ERROR_THROW(gptimer_enable(timer));
    enabled = true;
    ESP_ERROR_THROW(gptimer_set_raw_count(timer, 0));
    ESP_ERROR_THROW(gptimer_set_alarm_action(timer, &alarm));
    // Clear any stale signal from an earlier, timed-out run
    xSemaphoreTake(done, 0);

    // With interrupts disabled nothing can get between starting the pulse and starting the timer:
    // no other task can run and start a flash operation, and no ISR can run either.
    taskENTER_CRITICAL(&lock);
    start();
    esp_err_t startResult = gptimer_start(timer);
    taskEXIT_CRITICAL(&lock);
    started = startResult == ESP_OK;

    bool fired = startResult == ESP_OK
        && xSemaphoreTake(done, pdMS_TO_TICKS(std::chrono::duration_cast<std::chrono::milliseconds>(duration + ALARM_MARGIN).count())) == pdTRUE;
    if (!fired) {
        // Don't leave the pulse running
        holdChannelsHigh();
        LOGE("Pulse timer alarm did not fire (start result: %s), ended pulse from task",
            esp_err_to_name(startResult));
    }

    release();
    return fired;
}

void PulseTimer::release() noexcept {
    // Only undo what was done: the driver logs an error for stopping or disabling a timer twice
    if (started) {
        gptimer_stop(timer);
        started = false;
    }
    if (enabled) {
        gptimer_disable(timer);
        enabled = false;
    }
}

bool IRAM_ATTR PulseTimer::onAlarm(gptimer_handle_t /*timer*/, const gptimer_alarm_event_data_t* /*event*/, void* context) {
    auto* self = static_cast<PulseTimer*>(context);
    self->holdChannelsHigh();
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(self->done, &higherPriorityTaskWoken);
    return higherPriorityTaskWoken == pdTRUE;
}

void IRAM_ATTR PulseTimer::holdChannelsHigh() const {
    ledc_stop(speedMode, channelA, 1);
    ledc_stop(speedMode, channelB, 1);
}

}    // namespace cornucopia::ugly_duckling::kernel::drivers
