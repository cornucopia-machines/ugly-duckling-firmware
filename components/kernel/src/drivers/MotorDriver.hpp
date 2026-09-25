#pragma once

#include <Task.hpp>

#include <chrono>
#include <cstdint>

namespace cornucopia::ugly_duckling::kernel::drivers {

enum class MotorPhase : int8_t {
    Forward = 1,
    Reverse = -1
};

MotorPhase operator-(MotorPhase phase);

class PwmMotorDriver {
public:
    virtual ~PwmMotorDriver() = default;

    void stop() {
        drive(MotorPhase::Forward, 0);
    };

    virtual void drive(MotorPhase phase, double duty) = 0;

    /**
     * @brief Short the motor through the low-side FETs (slow decay), keeping the driver enabled.
     *
     * @details Use this to end an inductive pulse: the coil current recirculates inside the bridge
     * instead of flying back onto the supply rail, as it would when coasting. Call stop() afterwards
     * to release the driver.
     */
    virtual void brake() = 0;

    /**
     * @brief Drive the motor for `duration`, then brake, leaving the motor braking; call stop() to release the driver.
     *
     * @details The default implementation times the pulse from the calling task, so it can run longer
     * than `duration` when the task is delayed. Drivers that can time the end of the pulse in hardware
     * override this.
     */
    virtual void drivePulse(MotorPhase phase, double duty, std::chrono::milliseconds duration) {
        drive(phase, duty);
        Task::delay(duration);
        brake();
    }
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
