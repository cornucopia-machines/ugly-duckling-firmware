#pragma once

#include <functional>
#include <vector>

#include <Task.hpp>

namespace cornucopia::ugly_duckling::kernel {

class ShutdownManager {
public:
    void registerShutdownListener(const std::function<void()>& listener) {
        shutdownListeners.push_back(listener);
    }

    void startShutdown() {
        // Run in separate task to allocate enough stack
        Task::run("shutdown", 8192, [this](Task& _task) {
            // Notify all shutdown listeners
            for (auto& listener : shutdownListeners) {
                listener();
            }
            printf("Shutdown process finished\n");
        });
    }

private:
    std::vector<std::function<void()>> shutdownListeners;
};

}    // namespace cornucopia::ugly_duckling::kernel
