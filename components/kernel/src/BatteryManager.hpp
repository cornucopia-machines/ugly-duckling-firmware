#pragma once

#include <memory>

#include <esp_sleep.h>

#include <MovingAverage.hpp>
#include <ShutdownManager.hpp>
#include <Task.hpp>
#include <Telemetry.hpp>
#include <drivers/BatteryDriver.hpp>
#include <utility>

using namespace cornucopia::ugly_duckling::kernel::drivers;

namespace cornucopia::ugly_duckling::kernel {

/**
 * @brief Time to wait between battery checks.
 */
static constexpr microseconds LOW_POWER_SLEEP_CHECK_INTERVAL = 10s;

[[noreturn]] inline void enterLowPowerDeepSleep() {
    printf("Entering low power deep sleep\n");
    esp_deep_sleep(duration_cast<microseconds>(LOW_POWER_SLEEP_CHECK_INTERVAL).count());
    // Signal to the compiler that we are not returning for real
    abort();
}

class BatteryManager final {
public:
    BatteryManager(
        std::shared_ptr<BatteryDriver> battery,
        std::shared_ptr<ShutdownManager> shutdownManager)
        : battery(std::move(battery))
        , shutdownManager(std::move(shutdownManager)) {
        Task::loop("battery", 4096, [this](Task& task) {
            checkBatteryVoltage(task);
        });
    }

    /**
     * @brief Get the Voltage object
     *
     * @return int Battery voltage in millivolts, -1 if voltage cannot be determined,
     * or 0 if device has no battery.
     */
    int getVoltage() {
        return batteryVoltage.getAverage();
    }

    double getPercentage() {
        return battery->getPercentage();
    }

    std::optional<double> getCurrent() {
        return battery->getCurrent();
    }

    std::optional<seconds> getTimeToEmpty() {
        return battery->getTimeToEmpty();
    }

private:
    void checkBatteryVoltage(Task& task) {
                auto currentVoltage = battery->getVoltage();
        batteryVoltage.record(currentVoltage);
        auto voltage = batteryVoltage.getAverage();

        if (voltage != 0 && voltage < battery->parameters.shutdownThreshold) {
            LOGI("Battery voltage low (%d mV < %d mV), starting shutdown process, will go to deep sleep in %lld seconds",
                voltage, battery->parameters.shutdownThreshold, duration_cast<seconds>(LOW_BATTERY_SHUTDOWN_TIMEOUT).count());

            // TODO Publish all MQTT messages, then shut down WiFi, and _then_ start shutting down peripherals
            //      Doing so would result in less of a power spike, which can be important if the battery is already low

            shutdownManager->startShutdown();
            Task::delay(LOW_BATTERY_SHUTDOWN_TIMEOUT);
            enterLowPowerDeepSleep();
        }
        task.delayUntil(LOW_POWER_CHECK_INTERVAL);
    };

    const std::shared_ptr<BatteryDriver> battery;
    const std::shared_ptr<ShutdownManager> shutdownManager;

    MovingAverage<int> batteryVoltage { 5 };

    /**
     * @brief How often we check the battery voltage while in operation.
     *
     * We use a prime number to avoid synchronizing with other tasks.
     */
    static constexpr auto LOW_POWER_CHECK_INTERVAL = 10313ms;

    /**
     * @brief Time to wait for shutdown process to finish before going to deep sleep.
     */
    static constexpr auto LOW_BATTERY_SHUTDOWN_TIMEOUT = 10s;
};

}    // namespace cornucopia::ugly_duckling::kernel
