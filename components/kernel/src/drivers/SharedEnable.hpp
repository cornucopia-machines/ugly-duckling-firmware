#pragma once

#include <Log.hpp>
#include <Pin.hpp>
#include <Task.hpp>

#include <esp_timer.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace cornucopia::ugly_duckling::kernel::drivers {

/**
 * @brief Manages a shared binary output with multiple independent clients.
 *
 * The output is active when at least one client has acquired it, and
 * inactive when every client has released. Each client holds a Handle
 * obtained via createHandle().
 *
 * Whatever the output switches on may need time to come up before clients can use it: e.g. on
 * MK14 LOADEN also enables the TPS61378-Q1 load boost, which has a true load disconnect, so VLOAD
 * starts from 0 V every time (~0.4 ms enable delay plus ~2.5 ms soft-start). Loading the rail during
 * soft-start can trip the boost's short-circuit protection into hiccup mode, losing the whole pulse.
 * LOADEN is also the H-bridge's nSLEEP, so this also covers its wake-up time. Clients must keep their
 * outputs idle until acquire() returns, which only happens once the output has been active for at least
 * the settle time.
 */
class SharedEnable : public std::enable_shared_from_this<SharedEnable> {
public:
    class Handle {
    public:
        Handle() = default;

        Handle(Handle&& other) noexcept
            : parent(std::move(other.parent))
            , acquired(other.acquired.exchange(false)) {
        }

        Handle& operator=(Handle&& other) noexcept {
            if (this != &other) {
                release();
                parent = std::move(other.parent);
                acquired.store(other.acquired.exchange(false));
            }
            return *this;
        }

        ~Handle() {
            release();
        }

        /**
         * @brief Mark this handle as active, and wait until the output has settled; idempotent and thread-safe.
         *
         * @details Returns immediately if the output has been active for long enough already, e.g. because
         * another client acquired it earlier. A client acquiring during another client's settle window only
         * waits for the rest of it.
         */
        void acquire() {
            if (!parent) {
                return;
            }
            bool expected = false;
            if (acquired.compare_exchange_strong(expected, true)) {
                parent->handleAcquired();
            }
            parent->waitUntilSettled();
        }

        /**
         * @brief Mark this handle as inactive; idempotent and thread-safe.
         */
        void release() {
            bool expected = true;
            if (parent && acquired.compare_exchange_strong(expected, false)) {
                parent->handleReleased();
            }
        }

        bool isAcquired() const {
            return acquired.load();
        }

    private:
        friend class SharedEnable;

        explicit Handle(std::shared_ptr<SharedEnable> parent)
            : parent(std::move(parent)) {
        }

        std::shared_ptr<SharedEnable> parent;
        std::atomic<bool> acquired = false;
    };

    using Actuator = std::function<void(bool)>;

    /**
     * @param settleTime How long clients must wait after the output turns on before using what it enables.
     */
    SharedEnable(Actuator actuate, std::chrono::microseconds settleTime)
        : actuate(std::move(actuate))
        , settleTime(settleTime) {
    }

    /**
     * @brief Create a SharedEnable that does nothing on state changes.
     */
    static std::shared_ptr<SharedEnable> noOp() {
        return std::make_shared<SharedEnable>([](bool) { }, std::chrono::microseconds::zero());
    }

    /**
     * @brief Create a SharedEnable that drives a pin HIGH when active, LOW when inactive.
     */
    static std::shared_ptr<SharedEnable> forActiveHighPin(const PinPtr& pin, std::chrono::microseconds settleTime) {
        pin->pinMode(Pin::Mode::Output);
        pin->digitalWrite(0);
        return std::make_shared<SharedEnable>([pin](bool active) {
            pin->digitalWrite(active ? 1 : 0);
        },
            settleTime);
    }

    /**
     * @brief Create a SharedEnable that drives a pin LOW when active, HIGH when inactive.
     */
    static std::shared_ptr<SharedEnable> forActiveLowPin(const PinPtr& pin, std::chrono::microseconds settleTime) {
        pin->pinMode(Pin::Mode::Output);
        pin->digitalWrite(1);
        return std::make_shared<SharedEnable>([pin](bool active) {
            pin->digitalWrite(active ? 0 : 1);
        },
            settleTime);
    }

    Handle createHandle() {
        return Handle(shared_from_this());
    }

    /**
     * @brief Whether the output is active and has been for at least the settle time.
     *
     * @details Use it to ignore status signals (e.g. an H-bridge's nFAULT, asserted while VM is below UVLO)
     * that aren't meaningful while the enabled rail is still coming up.
     */
    bool isSettled() const {
        return readyAtMicros.load() != NOT_ACTIVE
            && remainingSettleTime() == std::chrono::microseconds::zero();
    }

private:
    /**
     * @brief Time left until the output has been active for the settle time; zero if it has, or if it is inactive.
     */
    std::chrono::microseconds remainingSettleTime() const {
        int64_t readyAt = readyAtMicros.load();
        if (readyAt == NOT_ACTIVE) {
            return std::chrono::microseconds::zero();
        }
        int64_t now = esp_timer_get_time();
        return std::chrono::microseconds(now < readyAt ? readyAt - now : 0);
    }

    void waitUntilSettled() const {
        while (true) {
            auto remaining = remainingSettleTime();
            if (remaining == std::chrono::microseconds::zero()) {
                return;
            }
            // vTaskDelay(n) can return after as little as n - 1 full ticks, hence the extra tick
            Task::delay(std::chrono::ceil<ticks>(remaining) + ticks(1));
        }
    }

    void handleAcquired() {
        std::scoped_lock lock(mutex);
        if (++activeCount == 1) {
            actuate(true);
            readyAtMicros.store(esp_timer_get_time() + settleTime.count());
        }
    }

    void handleReleased() {
        std::scoped_lock lock(mutex);
        if (--activeCount == 0) {
            readyAtMicros.store(NOT_ACTIVE);
            actuate(false);
        }
    }

    static constexpr int64_t NOT_ACTIVE = -1;

    Actuator actuate;
    const std::chrono::microseconds settleTime;
    std::atomic<int64_t> readyAtMicros = NOT_ACTIVE;
    int activeCount = 0;
    std::mutex mutex;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
