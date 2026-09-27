#pragma once

#include <Log.hpp>

#include <esp_attr.h>
#include <esp_err.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <chrono>
#include <cstdint>

namespace cornucopia::ugly_duckling::kernel {

/**
 * @brief Counts consecutive crash resets, so a crash loop is obvious from a single BOOT message.
 *
 * The count lives in `.noinit` DRAM, which survives panic and watchdog resets but not a power
 * cycle or deep sleep. It is incremented on every crash reset (panic, watchdog, CPU lockup),
 * reset to zero by any other kind of reset, and cleared once the device has stayed up for
 * `STABLE_UPTIME` -- a device that crashes once a week is not in a crash loop.
 */
class CrashCounter {
public:
    static constexpr std::chrono::minutes STABLE_UPTIME { 5 };

    /**
     * @brief Records this boot and returns the number of consecutive crash resets that led to it
     * (0 when the last reset was not a crash). Call once, early at boot.
     */
    static uint32_t recordBoot(esp_reset_reason_t resetReason) {
        uint32_t previousCount = state.magic == MAGIC
            ? state.count
            : 0;
        uint32_t count = isCrash(resetReason)
            ? previousCount + 1
            : 0;
        state = {
            .magic = MAGIC,
            .count = count,
        };

        if (count > 0) {
            esp_timer_create_args_t config = {
                .callback = [](void*) {
                    state.count = 0;
                },
                .arg = nullptr,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "crash-counter",
                .skip_unhandled_events = false,
            };
            esp_timer_handle_t timer = nullptr;
            esp_err_t err = esp_timer_create(&config, &timer);
            if (err == ESP_OK) {
                err = esp_timer_start_once(timer, std::chrono::duration_cast<std::chrono::microseconds>(STABLE_UPTIME).count());
            }
            if (err != ESP_OK) {
                LOGE("Failed to start crash counter timer: %s", esp_err_to_name(err));
            }
        }
        return count;
    }

private:
    static bool isCrash(esp_reset_reason_t resetReason) {
        switch (resetReason) {
            case ESP_RST_PANIC:
            case ESP_RST_INT_WDT:
            case ESP_RST_TASK_WDT:
            case ESP_RST_WDT:
            case ESP_RST_CPU_LOCKUP:
                return true;
            default:
                return false;
        }
    }

    // Distinguishes a count written by a previous boot from the random contents of
    // uninitialized RAM after a power cycle
    static constexpr uint32_t MAGIC = 0x43525348;    // "CRSH"

    struct State {
        uint32_t magic;
        uint32_t count;
    };

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    __NOINIT_ATTR static inline State state;
};

}    // namespace cornucopia::ugly_duckling::kernel
