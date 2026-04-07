#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include <Concurrent.hpp>
#include <Log.hpp>
#include <Pin.hpp>
#include <PowerManager.hpp>

using namespace std::chrono;

namespace farmhub::kernel {

LOGGING_TAG(PULSE, "pulse")

class PulseCounterManager;
static void handlePulseCounterInterrupt(void* arg);

struct PulseCounterConfig {
    InternalPinPtr pin;
    /**
     * @brief Ignore any pulses that happen within this time after the previous pulse.
     */
    microseconds debounceTime = 0us;
};

/**
 * @brief Counts pulses on a GPIO pin using interrupts.
 *
 * Note: This counter is safe to use with the device entering and exiting light sleep.
 *
 * When the device is awake, it watches for edges, and counts falling edges.
 * When the device enters light sleep, we set up an interrupt to wake on level change.
 * This is necessary because in light sleep the device cannot detect edges, only levels.
 */
class PulseCounter {
public:
    PulseCounter(const InternalPinPtr& pin, microseconds debounceTime)
        : pin(pin)
        , debounceTime(debounceTime)
        , lastEdge(pin->digitalRead())
        , lastCountedEdgeTime(steady_clock::now()) {
        auto gpio = pin->getGpio();

        // Configure the GPIO pin as an input
        gpio_config_t config = {
            .pin_bit_mask = 1ULL << gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        ESP_ERROR_THROW(gpio_config(&config));

        // Use the same configuration while in light sleep
        ESP_ERROR_THROW(gpio_sleep_sel_dis(gpio));

        // TODO Where should this be called?
        ESP_ERROR_THROW(esp_sleep_enable_gpio_wakeup());

        LOGTD(PULSE, "Registered interrupt-based pulse counter unit on pin %s",
            pin->getName().c_str());
    }

    uint32_t getCount() const {
        uint32_t count = edgeCount.load();
        LOGTV(PULSE, "Counted %" PRIu32 " pulses on pin %s",
            count, pin->getName().c_str());
        return count;
    }

    uint32_t reset() {
        uint32_t count = edgeCount.exchange(0);
        LOGTV(PULSE, "Counted %" PRIu32 " pulses and cleared on pin %s",
            count, pin->getName().c_str());
        return count;
    }

    PinPtr getPin() const {
        return pin;
    }

private:
    void handleGoingToLightSleep() {
        auto currentState = pin->digitalRead();
        // Make sure we wake up again to check for the opposing edge
        ESP_ERROR_CHECK(gpio_wakeup_enable(
            pin->getGpio(),
            currentState == 0
                ? GPIO_INTR_HIGH_LEVEL
                : GPIO_INTR_LOW_LEVEL));
    }

    void handleWakingUpFromLightSleep() {
        // Switch back to edge detection when we are awake
        ESP_ERROR_CHECK(gpio_wakeup_disable(pin->getGpio()));
        ESP_ERROR_CHECK(gpio_set_intr_type(pin->getGpio(), GPIO_INTR_ANYEDGE));

        handlePulseCounterInterrupt(this);
    }

    const InternalPinPtr pin;
    const microseconds debounceTime;
    std::atomic<uint32_t> edgeCount { 0 };
    int lastEdge;
    steady_clock::time_point lastCountedEdgeTime;

    friend void handlePulseCounterInterrupt(void* arg);
    friend class PulseCounterManager;
};

static void IRAM_ATTR handlePulseCounterInterrupt(void* arg) {
    auto* counter = static_cast<PulseCounter*>(arg);
    auto currentState = counter->pin->digitalReadFromISR();
    if (currentState != counter->lastEdge) {
        counter->lastEdge = currentState;

        // Software debounce: ignore edges that happen too quickly
        if (counter->debounceTime > 0us) {
            auto now = steady_clock::now();
            auto timeSinceLastEdge = duration_cast<microseconds>(now - counter->lastCountedEdgeTime);
            if (timeSinceLastEdge < counter->debounceTime) {
                return;
            }
            counter->lastCountedEdgeTime = now;
        }

        if (currentState == 0) {
            counter->edgeCount++;
        }
    }
}

class PulseCounterManager {
public:
    std::shared_ptr<PulseCounter> create(const PulseCounterConfig& config) {
        if (!initialized) {
            initialized = true;

            // Make sure we handle any state changes when the device wakes up due to a GPIO interrupt
            esp_pm_sleep_cbs_register_config_t sleepCallbackConfig = {
                .enter_cb = [](int64_t _timeToSleepInUs, void* arg) {
                    auto* self = static_cast<PulseCounterManager*>(arg);
                    for (auto& counter : self->counters) {
                        counter->handleGoingToLightSleep();
                    }
                    return ESP_OK; },
                .exit_cb = [](int64_t _timeSleptInUs, void* arg) {
                    auto* self = static_cast<PulseCounterManager*>(arg);
                    for (auto& counter : self->counters) {
                        counter->handleWakingUpFromLightSleep();
                    }
                    return ESP_OK; },
                .enter_cb_user_arg = this,
                .exit_cb_user_arg = this,
                .enter_cb_prior = 0,
                .exit_cb_prior = 0,
            };
            ESP_ERROR_THROW(esp_pm_light_sleep_register_cbs(&sleepCallbackConfig));
        }

        auto counter = std::make_shared<PulseCounter>(config.pin, config.debounceTime);

        // Attach the ISR handler to the GPIO pin
        ESP_ERROR_THROW(gpio_isr_handler_add(config.pin->getGpio(), handlePulseCounterInterrupt, counter.get()));

        // Keep the counter alive in the manager
        counters.push_back(counter);
        return counter;
    }

private:
    bool initialized = false;
    std::vector<std::shared_ptr<PulseCounter>> counters;
};

}    // namespace farmhub::kernel
