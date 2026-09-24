#pragma once

#include <Log.hpp>
#include <Task.hpp>

#include <esp_system.h>

#include <cstdio>

namespace cornucopia::ugly_duckling::kernel {

/**
 * @brief Flushes stdout, delays briefly to let MQTT messages reach the broker, then restarts.
 *
 * All application-level restarts should go through this function so the flush-and-delay logic
 * lives in one place — when we eventually replace the fixed 5s delay with something smarter
 * (e.g. waiting for the MQTT outbox to drain), only this function needs to change.
 */
[[noreturn]] inline void delayedRestart() {
    // Restarts often happen at the end of a deep call chain (e.g. applying an MQTT `update`),
    // so this is where the calling task's peak stack usage is known
    LOGD("Restarting from task '%s', stack high-water mark: %u bytes unused",
        pcTaskGetName(nullptr), static_cast<unsigned int>(uxTaskGetStackHighWaterMark(nullptr)));
    (void) fflush(stdout);
    fsync(fileno(stdout));
    Task::delay(5s);
    esp_restart();
    __builtin_unreachable();
}

}    // namespace cornucopia::ugly_duckling::kernel
