#include "Motors.hpp"
#include "PeripheralException.hpp"
#include "drivers/MotorDriver.hpp"
#include <map>
#include <memory>
#include <string>

namespace farmhub::peripherals {

std::shared_ptr<PwmMotorDriver> findMotor(
    const std::map<std::string, std::shared_ptr<PwmMotorDriver>>& motors,
    const std::string& motorName) {

    if (motorName.empty() && motors.size() == 1) {
        return motors.begin()->second;
    }
    for (const auto& m : motors) {
        if (m.first == motorName) {
            return m.second;
        }
    }
    throw PeripheralCreationException("failed to find motor: " + motorName);
}

}    // namespace farmhub::peripherals
