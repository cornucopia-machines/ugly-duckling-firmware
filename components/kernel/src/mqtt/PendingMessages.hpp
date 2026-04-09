#pragma once

#include <unordered_map>

#include <Concurrent.hpp>
#include <Task.hpp>

namespace cornucopia::ugly_duckling::kernel::mqtt {

enum class PublishStatus : uint8_t {
    TimeOut = 0,
    Success = 1,
    Failed = 2,
    Pending = 3,
    QueueFull = 4
};

static constexpr uint32_t PUBLISH_SUCCESS = 1;
static constexpr uint32_t PUBLISH_FAILED = 2;

class PendingMessages {
public:
    bool waitOn(int messageId, TaskHandle_t waitingTask) {
        if (waitingTask == nullptr) {
            // Nothing is waiting
            return false;
        }

        if (messageId == 0) {
            // Notify tasks waiting on QoS 0 messages immediately
            notifyWaitingTask(waitingTask, true);
            return false;
        }

        // Record pending task
        Lock lock(mutex);
        messages[messageId] = waitingTask;
        return true;
    }

    bool handlePublished(int messageId, bool success) {
        if (messageId == 0) {
            return false;
        }

        Lock lock(mutex);
        auto it = messages.find(messageId);
        if (it != messages.end()) {
            notifyWaitingTask(it->second, success);
            messages.erase(it);
            return true;
        }
        return false;
    }

    bool cancelWaitingOn(TaskHandle_t waitingTask) {
        if (waitingTask == nullptr) {
            return false;
        }

        Lock lock(mutex);
        bool removed = false;
        for (auto it = messages.begin(); it != messages.end();) {
            if (it->second == waitingTask) {
                it = messages.erase(it);
                removed = true;
            } else {
                ++it;
            }
        }
        return removed;
    }

    void clear() {
        Lock lock(mutex);
        for (auto& [messageId, waitingTask] : messages) {
            notifyWaitingTask(waitingTask, false);
        }
        messages.clear();
    }

    static void notifyWaitingTask(TaskHandle_t task, bool success) {
        if (task != nullptr) {
            auto status = success ? PublishStatus::Success : PublishStatus::Failed;
            xTaskNotify(task, static_cast<int>(status), eSetValueWithOverwrite);
        }
    }

private:
    Mutex mutex;
    std::unordered_map<int, TaskHandle_t> messages;
};

}    // namespace cornucopia::ugly_duckling::kernel::mqtt
