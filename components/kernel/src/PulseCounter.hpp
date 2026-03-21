#pragma once

#include <atomic>
#include <chrono>
#include <list>
#include <memory>

#include <driver/gpio.h>
#include <esp_sleep.h>

#include <Concurrent.hpp>
#include <EspException.hpp>
#include <Log.hpp>
#include <Pin.hpp>

using namespace std::chrono;

namespace farmhub::kernel {

LOGGING_TAG(PULSE, "pulse")

class PulseCounterManager;

struct PulseCounterConfig {
    InternalPinPtr pin;
    /**
     * @brief Ignore any pulses that happen within this time after the previous pulse.
     */
    microseconds debounceTime = 0us;
};

/**
 * @brief Abstract interface for pulse counters.
 *
 * Created via PulseCounterManager::create(), which selects the appropriate implementation:
 * - UlpPulseCounter for RTC-capable GPIOs (GPIO 0–21 on ESP32-S3): counts via the ULP-RISC-V
 *   coprocessor, requiring no CPU wakeups during light sleep.
 * - GpioPulseCounter for all other GPIOs: counts via GPIO interrupts, light-sleep aware.
 */
class PulseCounter {
public:
    virtual uint32_t getCount() const = 0;
    virtual uint32_t reset() = 0;
    virtual PinPtr getPin() const = 0;
    virtual ~PulseCounter() = default;

protected:
    PulseCounter() = default;
};

// ============================================================================
// GpioPulseCounter — GPIO interrupt path, light-sleep aware
// ============================================================================

class GpioPulseCounter;
static void handleGpioPulseCounterInterrupt(void* arg);

/**
 * @brief Counts pulses on a GPIO pin using interrupts.
 *
 * Note: This counter is safe to use with the device entering and exiting light sleep.
 *
 * When the device is awake, it watches for edges, and counts falling edges.
 * When the device enters light sleep, we set up an interrupt to wake on level change.
 * This is necessary because in light sleep the device cannot detect edges, only levels.
 *
 * Use this for GPIOs that are not RTC-capable. For RTC-capable GPIOs, prefer
 * UlpPulseCounter (selected automatically by PulseCounterManager).
 */
class GpioPulseCounter final : public PulseCounter {
public:
    GpioPulseCounter(const InternalPinPtr& pin, microseconds debounceTime)
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

        LOGTD(PULSE, "Registered interrupt-based pulse counter on pin %s",
            pin->getName().c_str());
    }

    uint32_t getCount() const override {
        uint32_t count = edgeCount.load();
        LOGTV(PULSE, "Counted %" PRIu32 " pulses on pin %s",
            count, pin->getName().c_str());
        return count;
    }

    uint32_t reset() override {
        uint32_t count = edgeCount.exchange(0);
        LOGTV(PULSE, "Counted %" PRIu32 " pulses and cleared on pin %s",
            count, pin->getName().c_str());
        return count;
    }

    PinPtr getPin() const override {
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
        handleGpioPulseCounterInterrupt(this);
    }

    const InternalPinPtr pin;
    const microseconds debounceTime;
    std::atomic<uint32_t> edgeCount { 0 };
    int lastEdge;
    steady_clock::time_point lastCountedEdgeTime;

    friend void handleGpioPulseCounterInterrupt(void* arg);
    friend class PulseCounterManager;
};

static void IRAM_ATTR handleGpioPulseCounterInterrupt(void* arg) {
    auto* counter = static_cast<GpioPulseCounter*>(arg);
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

// ============================================================================
// PulseCounterManager
// ============================================================================

/**
 * @brief Creates and manages pulse counters, automatically selecting the implementation.
 *
 * For RTC/LP-capable GPIOs: creates a UlpPulseCounter (coprocessor-based, zero CPU wakeups).
 *   - ESP32-S3: GPIO 0–21 (ULP-RISC-V, ~17.5 MHz)
 *   - ESP32-C6: GPIO 0–7  (LP Core, ~16 MHz)
 * For all other GPIOs: creates a GpioPulseCounter (interrupt-based, light-sleep aware).
 *
 * Call start() after all counters have been created to load and start the coprocessor firmware.
 * start() is a no-op if no ULP counters were created.
 */
class PulseCounterManager {
public:
    std::shared_ptr<PulseCounter> create(const PulseCounterConfig& config);

    /**
     * @brief Load and start the ULP/LP-core firmware.
     *
     * Must be called after all ULP channels are registered (i.e., after all create() calls
     * for RTC/LP-capable GPIOs). Safe to call even if no ULP channels were created (no-op).
     * Calling more than once is also a no-op.
     */
    void start();

private:
    // GPIO interrupt path
    bool gpioInitialized = false;
    std::list<GpioPulseCounter*> gpioCounters;

    std::list<std::shared_ptr<PulseCounter>> ulpCounters;

#ifdef CONFIG_ULP_COPROC_ENABLED
    static constexpr uint32_t ULP_MAX_CHANNELS = 4;

    // ULP/LP-core path
    bool ulpStarted = false;
    uint32_t ulpNextChannel = 0;

    struct UlpChannelConfig {
        uint32_t gpioNum;
        uint32_t debounceUs;
    };
    UlpChannelConfig ulpChannelConfigs[ULP_MAX_CHANNELS] = {};

    std::shared_ptr<PulseCounter> createUlp(const PulseCounterConfig& config);
#endif    // CONFIG_ULP_COPROC_ENABLED

    std::shared_ptr<PulseCounter> createGpio(const PulseCounterConfig& config);
};

}    // namespace farmhub::kernel
