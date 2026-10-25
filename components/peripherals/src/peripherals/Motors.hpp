#pragma once

#include <Result.hpp>
#include <drivers/MotorDriver.hpp>

#include <map>
#include <memory>
#include <string>

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::drivers;

namespace cornucopia::ugly_duckling::peripherals {

Result<std::shared_ptr<PwmMotorDriver>> findMotor(
    const std::map<std::string, std::shared_ptr<PwmMotorDriver>>& motors,
    const std::string& motorName);

}    // namespace cornucopia::ugly_duckling::peripherals
