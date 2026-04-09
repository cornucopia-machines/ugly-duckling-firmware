#pragma once

#include <chrono>
#include <functional>

#include <esp_check.h>
#include <esp_timer.h>

#include <Log.hpp>
#include <utility>

using namespace std::chrono;
using namespace std::chrono_literals;

namespace cornucopia::ugly_duckling::kernel {

enum class WatchdogState : uint8_t {
    Started,
    Cancelled,
    TimedOut
};

using WatchdogCallback = std::function<void(WatchdogState)>;

class Watchdog {
public:
    Watchdog(const std::string& name, const microseconds timeout, bool startImmediately, WatchdogCallback callback)
        : name(name)
        , timeout(timeout)
        , callback(std::move(callback)) {
        esp_timer_create_args_t config = {
            .callback = [](void* arg) {
                auto* watchdog = static_cast<Watchdog*>(arg);
                watchdog->callback(WatchdogState::TimedOut);
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = this->name.c_str(),
            .skip_unhandled_events = false,
        };
        esp_err_t ret = esp_timer_create(&config, &timer);
        if (ret != ESP_OK) {
            LOGE("Failed to create watchdog timer: %s", esp_err_to_name(ret));
            esp_system_abort("Failed to create watchdog timer");
        }
        if (startImmediately) {
            restart();
        }
    }

    ~Watchdog() {
        cancel();
        esp_timer_delete(timer);
    }

    bool restart() {
        // TODO Add proper error handling
        if (esp_timer_restart(timer, timeout.count()) == ESP_ERR_INVALID_STATE) {
            esp_timer_start_once(timer, timeout.count());
        }
        callback(WatchdogState::Started);
        return true;
    }

    bool cancel() {
        // TODO Add proper error handling
        if (esp_timer_stop(timer) == ESP_OK) {
            callback(WatchdogState::Cancelled);
            return true;
        }
        return false;
    }

private:
    const std::string name;
    const microseconds timeout;
    const WatchdogCallback callback;
    esp_timer_handle_t timer {};
};

}    // namespace cornucopia::ugly_duckling::kernel
