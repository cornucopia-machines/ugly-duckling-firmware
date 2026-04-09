#include "MotorDriver.hpp"

namespace cornucopia::ugly_duckling::kernel::drivers {

MotorPhase operator-(MotorPhase phase) {
    return phase == MotorPhase::Forward
        ? MotorPhase::Reverse
        : MotorPhase::Forward;
}

}    // namespace cornucopia::ugly_duckling::kernel::drivers
