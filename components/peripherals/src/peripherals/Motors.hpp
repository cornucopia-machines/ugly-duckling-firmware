#pragma once

#include <map>
#include <memory>
#include <string>

#include <drivers/MotorDriver.hpp>

using namespace cornucopia::ugly_duckling::kernel::drivers;

namespace cornucopia::ugly_duckling::peripherals {

std::shared_ptr<PwmMotorDriver> findMotor(
    const std::map<std::string, std::shared_ptr<PwmMotorDriver>>& motors,
    const std::string& motorName);

}    // namespace cornucopia::ugly_duckling::peripherals
