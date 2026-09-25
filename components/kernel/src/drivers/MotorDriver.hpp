#pragma once

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
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
