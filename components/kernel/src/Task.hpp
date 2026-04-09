#pragma once

#include <chrono>
#include <functional>

#include <freertos/FreeRTOS.h>    // NOLINT(misc-header-include-cycle)
#include <freertos/task.h>        // NOLINT(misc-header-include-cycle)

#include <Log.hpp>
#include <Time.hpp>
#include <utility>

using namespace std::chrono;

namespace cornucopia::ugly_duckling::kernel {

static const unsigned int DEFAULT_PRIORITY = 1;

class Task;

using TaskFunction = std::function<void(Task&)>;

class TaskHandle {
public:
    TaskHandle(TaskHandle_t handle)
        : handle(handle) {
    }

    TaskHandle()
        : TaskHandle(nullptr) {
    }

    TaskHandle(const TaskHandle& other)
        : TaskHandle(other.handle) {
    }

    TaskHandle& operator=(const TaskHandle& other) {
        if (this != &other) {    // Self-assignment check
            handle = other.handle;
        }
        return *this;
    }

    constexpr bool isValid() const {
        return handle != nullptr;
    }

    void suspend() {
        if (isValid()) {
            vTaskSuspend(handle);
        }
    }

    void resume() {
        if (isValid()) {
            vTaskResume(handle);
        }
    }

    bool abortDelay() {
        if (isValid()) {
            return xTaskAbortDelay(handle) != 0;
        }
        return false;
    }

private:
    TaskHandle_t handle;
};

class Task {
public:
    static TaskHandle run(const std::string& name, uint32_t stackSize, const TaskFunction& runFunction) {
        return Task::run(name, stackSize, DEFAULT_PRIORITY, runFunction);
    }
    static TaskHandle run(const std::string& name, uint32_t stackSize, UBaseType_t priority, const TaskFunction& runFunction) {
        auto* taskFunction = new TaskFunction(runFunction);
        LOGD("Creating task %s with priority %u and stack size %" PRIu32,
            name.c_str(), priority, stackSize);
        TaskHandle_t handle = nullptr;
        auto result = xTaskCreate(executeTask, name.c_str(), stackSize, taskFunction, priority, &handle);
        if (result != pdPASS) {
            LOGE("Failed to create task %s: %d", name.c_str(), result);
            delete taskFunction;
            return {};
        }
        return TaskHandle { handle };
    }

    static TaskHandle loop(const std::string& name, uint32_t stackSize, const TaskFunction& loopFunction) {
        return Task::loop(name, stackSize, DEFAULT_PRIORITY, loopFunction);
    }
    static TaskHandle loop(const std::string& name, uint32_t stackSize, UBaseType_t priority, const TaskFunction& loopFunction) {
        return Task::run(name, stackSize, priority, [loopFunction](Task& task) {
            while (true) {
                loopFunction(task);
            }
        });
    }

    static void delay(ticks time) {
        // LOGV("Task '%s' delaying for %lld ms",
        //     pcTaskGetName(nullptr), duration_cast<milliseconds>(time).count());
        vTaskDelay(time.count());
    }

    bool delayUntil(ticks time) {
        if (delayUntilAtLeast(time)) {
            return true;
        }
        auto newWakeTime = xTaskGetTickCount();
        printf("Task '%s' missed deadline by %lld ms\n",
            pcTaskGetName(nullptr), duration_cast<milliseconds>(ticks(newWakeTime - lastWakeTime)).count());
        lastWakeTime = newWakeTime;
        return false;
    }

    bool delayUntilAtLeast(ticks time) {
        // LOGV("Task '%s' delaying until %lld ms",
        //     pcTaskGetName(nullptr), duration_cast<milliseconds>(time).count());
        return xTaskDelayUntil(&lastWakeTime, time.count()) != 0;
    }

    /**
     * @brief Ticks to wait until the given `time` since last task wake time.
     *
     * @param time The time period the caller wants elapsed since the last wake time.
     * @return ticks The number of ticks to delay until the given `time` has elapsed since the last wake time,
     *     or zero if the time has already elapsed.
     */
    ticks ticksUntil(ticks time) const {
        auto currentTime = ticks(xTaskGetTickCount());
        // Handling tick overflow. If 'currentTime' is less than 'lastWakeTime',
        // it means the tick count has rolled over.
        if (currentTime - ticks(lastWakeTime) < time) {
            // This means 'targetTime' is still in the future, taking into account possible overflow.
            return time - (currentTime - ticks(lastWakeTime));
        }
        // 'currentTime' has surpassed our target time, indicating the delay has expired.
        // printf("Task '%s' is already past deadline by %lld ms\n",
        //     pcTaskGetName(nullptr), duration_cast<milliseconds>(currentTime - ticks(lastWakeTime)).count());
        return ticks::zero();
    }

    /**
     * @brief Mark the current time as the last wake time.
     */
    void markWakeTime() {
        lastWakeTime = xTaskGetTickCount();
    }

    static void suspend() {
        vTaskSuspend(nullptr);
    }

    static void yield() {
        taskYIELD();
    }

private:
    ~Task() {
        LOGV("Finished task %s\n",
            pcTaskGetName(nullptr));
        vTaskDelete(nullptr);
    }

    static void executeTask(void* parameters) {
        auto* taskFunction = static_cast<TaskFunction*>(parameters);
        Task task;
        (*taskFunction)(task);
        delete taskFunction;
    }

    TickType_t lastWakeTime { xTaskGetTickCount() };
};

}    // namespace cornucopia::ugly_duckling::kernel
