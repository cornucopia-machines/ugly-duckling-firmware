#pragma once

#include <Log.hpp>
#include <Pin.hpp>
#include <PowerManager.hpp>
#include <PwmManager.hpp>
#include <drivers/SharedEnable.hpp>

#include <esp_timer.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <ratio>

using namespace std::chrono;

namespace cornucopia::ugly_duckling::kernel::drivers {

/**
 * @brief Driver for a passive buzzer via PWM.
 *
 * Drives a passive buzzer with a square wave at a fixed frequency.
 * Volume is controlled by the duty cycle (0.5 = maximum for a passive buzzer).
 */
class BuzzerDriver {
public:
    BuzzerDriver(
        const std::shared_ptr<PwmManager>& pwm,
        const InternalPinPtr& buzzerPin,
        const std::shared_ptr<SharedEnable>& enable)
        : channel(pwm->registerPin(buzzerPin, BUZZER_FREQ, BUZZER_RES))
        , enableHandle(enable->createHandle()) {

        LOGI("Initializing buzzer on pin %s at %" PRIu32 " Hz",
            buzzerPin->getName().c_str(),
            BUZZER_FREQ);

        esp_timer_create_args_t timerArgs = {
            .callback = [](void* arg) {
                static_cast<BuzzerDriver*>(arg)->stop();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "buzzer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_THROW(esp_timer_create(&timerArgs, &stopTimer));
    }

    ~BuzzerDriver() {
        esp_timer_stop(stopTimer);
        try {
            stop();
        } catch (...) {
            LOGE("Failed to stop buzzer: %s",
                std::current_exception().__cxa_exception_type()->name());
        }
        esp_timer_delete(stopTimer);
    }

    /**
     * @brief Sound the buzzer for the given duration.
     *
     * @param duration How long to buzz.
     * @param duty PWM duty cycle (0.0–1.0). 0.5 gives maximum volume
     *             for a passive buzzer. Default is 0.5.
     *
     * If called while a previous buzz is still playing, the timer is
     * restarted with the new duration.
     */
    void buzz(milliseconds duration, double duty = 0.5) {
        std::scoped_lock lock(mutex);
        esp_timer_stop(stopTimer);

        enableHandle.acquire();
        sleepLock.emplace(PowerManager::noLightSleep);
        auto dutyValue = static_cast<uint32_t>(channel.maxValue() * duty);
        channel.write(dutyValue);

        LOGI("Buzzing for %lld ms at %.0f%% duty",
            static_cast<long long>(duration.count()),
            duty * 100);

        ESP_ERROR_THROW(esp_timer_start_once(stopTimer,
            duration_cast<std::chrono::duration<uint64_t, std::micro>>(duration).count()));
    }

private:
    void stop() {
        std::scoped_lock lock(mutex);
        channel.write(0);
        enableHandle.release();
        sleepLock.reset();
    }

    static constexpr uint32_t BUZZER_FREQ = 2700;
    static constexpr ledc_timer_bit_t BUZZER_RES = LEDC_TIMER_8_BIT;

    std::mutex mutex;
    PwmPin& channel;
    SharedEnable::Handle enableHandle;
    esp_timer_handle_t stopTimer = nullptr;
    std::optional<PowerManagementLockGuard> sleepLock;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
