#pragma once

#include <Log.hpp>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include <algorithm>
#include <string>
#include <vector>

namespace cornucopia::ugly_duckling::kernel {

/**
 * @brief Logs the stack high-water mark of every task, including ESP-IDF's own, at debug level.
 *
 * Task stacks are sized by estimate and all come out of internal RAM, which is tight on ESP32-C6.
 * The high-water mark (on ESP-IDF, in bytes) is the least stack space a task has had unused since
 * it started, so tasks with a large value are candidates for a smaller stack; tasks near zero are
 * about to overflow. Sorted with the least headroom first.
 *
 * Needs CONFIG_FREERTOS_USE_TRACE_FACILITY for uxTaskGetSystemState(), which only debug builds
 * enable (sdkconfig.debug.defaults); a no-op otherwise.
 */
inline void logTaskStackHighWaterMarks() {
#ifdef CONFIG_FREERTOS_USE_TRACE_FACILITY
    // Room for a couple of tasks created between the count and the snapshot
    std::vector<TaskStatus_t> tasks(uxTaskGetNumberOfTasks() + 2);
    tasks.resize(uxTaskGetSystemState(tasks.data(), tasks.size(), nullptr));
    std::ranges::sort(tasks, [](const TaskStatus_t& a, const TaskStatus_t& b) {
        return a.usStackHighWaterMark < b.usStackHighWaterMark;
    });

    std::string summary;
    for (const auto& task : tasks) {
        if (!summary.empty()) {
            summary += ", ";
        }
        summary += task.pcTaskName;
        summary += '=';
        summary += std::to_string(task.usStackHighWaterMark);
    }
    LOGD("Task stack high-water marks (bytes unused): %s", summary.c_str());
#endif
}

}    // namespace cornucopia::ugly_duckling::kernel
