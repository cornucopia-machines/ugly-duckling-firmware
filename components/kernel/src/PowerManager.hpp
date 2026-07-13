#pragma once

#include <array>
#include <chrono>

#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_timer.h>

#include <Concurrent.hpp>
#include <EspException.hpp>
#include <Log.hpp>
#include <Task.hpp>
#include <Telemetry.hpp>

#if defined(CONFIG_IDF_TARGET_ESP32S2)
// Apparently on ESP32S2 things start to break down if we go below 80 MHz
#define MIN_CPU_FREQ_MHZ 80
#elif defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C6)
#define MIN_CPU_FREQ_MHZ CONFIG_XTAL_FREQ
#else
#error "Target not supported " CONFIG_IDF_TARGET
#endif

using namespace std::chrono;

namespace cornucopia::ugly_duckling::kernel {

LOGGING_TAG(PM, "pm")

class PowerManagementLock {
public:
    PowerManagementLock(const std::string& name, esp_pm_lock_type_t type)
        : name(name) {
        ESP_ERROR_THROW(esp_pm_lock_create(type, 0, name.c_str(), &lock));
    }

    ~PowerManagementLock() {
        ESP_ERROR_CHECK(esp_pm_lock_delete(lock));
    }

    // Delete copy constructor and assignment operator to prevent copying
    PowerManagementLock(const PowerManagementLock&) = delete;
    PowerManagementLock& operator=(const PowerManagementLock&) = delete;

private:
    const std::string name;
    esp_pm_lock_handle_t lock = nullptr;

    friend class PowerManagementLockGuard;
};

class PowerManagementLockGuard {
public:
    PowerManagementLockGuard(PowerManagementLock& lock)
        : lock(lock) {
        ESP_ERROR_THROW(esp_pm_lock_acquire(lock.lock));
    }

    ~PowerManagementLockGuard() {
        if (lock.lock != nullptr) {
            ESP_ERROR_CHECK(esp_pm_lock_release(lock.lock));
        }
    }

    // Delete copy constructor and assignment operator to prevent copying
    PowerManagementLockGuard(const PowerManagementLockGuard&) = delete;
    PowerManagementLockGuard& operator=(const PowerManagementLockGuard&) = delete;

private:
    PowerManagementLock& lock;
};

class PowerManager final {
public:
    PowerManager(bool requestedSleepWhenIdle)
        : sleepWhenIdle(shouldSleepWhenIdle(requestedSleepWhenIdle)) {

        LOGTV(PM, "Configuring power management, CPU max/min at %d/%d MHz, light sleep is %s",
            CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, MIN_CPU_FREQ_MHZ, sleepWhenIdle ? "enabled" : "disabled");
        esp_pm_config_t pm_config = {
            .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
            .min_freq_mhz = MIN_CPU_FREQ_MHZ,
            .light_sleep_enable = sleepWhenIdle,
        };
        ESP_ERROR_THROW(esp_pm_configure(&pm_config));

#ifdef CONFIG_PM_LIGHT_SLEEP_CALLBACKS
        esp_pm_sleep_cbs_register_config_t cbs_conf = {
            .enter_cb = nullptr,
            .exit_cb = [](int64_t timeSleptInUs, void* arg) {
                auto* self = static_cast<PowerManager*>(arg);
                self->lightSleepTime += microseconds(timeSleptInUs);
                self->lightSleepCount++;
#ifdef UD_PM_DIAGNOSTICS
                // Tally exact wakeup cause(s) per cycle. Bitmask, so a wake can be
                // attributed to more than one cause at once.
                uint32_t causes = esp_sleep_get_wakeup_causes();
                for (size_t i = 0; i < self->wakeupCauseCounts.size(); i++) {
                    if (causes & (1u << i)) {
                        self->wakeupCauseCounts[i]++;
                    }
                }
#endif
                return ESP_OK;
            },
            .enter_cb_user_arg = nullptr,
            .exit_cb_user_arg = this,
            .enter_cb_prior = 0,
            .exit_cb_prior = 0,
        };
        ESP_ERROR_THROW(esp_pm_light_sleep_register_cbs(&cbs_conf));
#endif

#ifdef UD_PM_DIAGNOSTICS
        // Dumps PM locks, esp_timer stats, light-sleep wakeup count/causes, and (if
        // CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) per-task CPU time every 2.5s. Real
        // overhead (~9% CPU in testing) — only active in UD_PM_DIAGNOSTICS builds.
        Task::loop("power-manager", 4096, [this](Task& task) {
            esp_pm_dump_locks(stdout);
            esp_timer_dump(stdout);
#ifdef CONFIG_PM_LIGHT_SLEEP_CALLBACKS
            printf("Light sleep: %d wakeups, %lld us slept (since last dump)\n",
                lightSleepCount, static_cast<long long>(lightSleepTime.count()));
            lightSleepCount = 0;
            lightSleepTime = microseconds::zero();
            printf("Wakeup causes (since last dump): TIMER=%lu WIFI=%lu BT=%lu UART=%lu GPIO=%lu OTHER_BITS=",
                wakeupCauseCounts[ESP_SLEEP_WAKEUP_TIMER],
                wakeupCauseCounts[ESP_SLEEP_WAKEUP_WIFI],
                wakeupCauseCounts[ESP_SLEEP_WAKEUP_BT],
                wakeupCauseCounts[ESP_SLEEP_WAKEUP_UART],
                wakeupCauseCounts[ESP_SLEEP_WAKEUP_GPIO]);
            for (size_t i = 0; i < wakeupCauseCounts.size(); i++) {
                if (wakeupCauseCounts[i] != 0
                    && i != ESP_SLEEP_WAKEUP_TIMER && i != ESP_SLEEP_WAKEUP_WIFI
                    && i != ESP_SLEEP_WAKEUP_BT && i != ESP_SLEEP_WAKEUP_UART && i != ESP_SLEEP_WAKEUP_GPIO) {
                    printf("[bit%u]=%lu ", static_cast<unsigned>(i), wakeupCauseCounts[i]);
                }
            }
            printf("\n");
            wakeupCauseCounts.fill(0);
#endif
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
            static char buffer[2048];
            vTaskGetRunTimeStats(buffer);
            printf("Task Name\tState\tPrio\tStack\tNum\n");
            printf("%s\n", buffer);
#endif
            Task::delay(2500ms);
        });
#endif
    }
    const bool sleepWhenIdle;

    void populateTelemetry(JsonObject& json) {
#ifdef CONFIG_PM_LIGHT_SLEEP_CALLBACKS
        auto now = steady_clock::now();
        auto duration = duration_cast<microseconds>(now - sleepTimeLastReported);
        if (duration.count() > 0) {
            double currentLightSleepRatio = static_cast<double>(lightSleepTime.count()) / static_cast<double>(duration.count());
            auto currentLightSleepCount = lightSleepCount;
            sleepTimeLastReported = now;
            lightSleepTime = microseconds::zero();
            lightSleepCount = 0;
            json["sleep-ratio"] = currentLightSleepRatio;
            json["sleep-count"] = currentLightSleepCount;
        }
#endif
    }

    static PowerManagementLock noLightSleep;

private:
    static bool shouldSleepWhenIdle(bool requestedSleepWhenIdle) {
        if (requestedSleepWhenIdle) {
#if UD_DEBUG
            LOGTI(PM, "Light sleep is disabled in debug mode");
            return false;
#elif not(CONFIG_PM_ENABLE)
            LOGTI(PM, "Power management is disabled because CONFIG_PM_ENABLE is not set");
            return false;
#elif not(CONFIG_FREERTOS_USE_TICKLESS_IDLE)
            LOGTI(PM, "Light sleep is disabled because CONFIG_FREERTOS_USE_TICKLESS_IDLE is not set");
            return false;
#else
            LOGTI(PM, "Light sleep is enabled");
            return true;
#endif
        } else {
            LOGTI(PM, "Light sleep is disabled");
            return false;
        }
    }

#ifdef CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    steady_clock::time_point sleepTimeLastReported = steady_clock::now();
    microseconds lightSleepTime = microseconds::zero();
    int lightSleepCount = 0;
#ifdef UD_PM_DIAGNOSTICS
    // Per-wakeup-cause tally, indexed by esp_sleep_wakeup_cause_t bit position
    std::array<unsigned long, 32> wakeupCauseCounts = { };
#endif
#endif
};


}    // namespace cornucopia::ugly_duckling::kernel
