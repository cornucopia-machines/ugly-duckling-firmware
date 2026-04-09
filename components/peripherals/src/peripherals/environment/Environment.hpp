#pragma once

#include <concepts>
#include <memory>

#include <Configuration.hpp>
#include <I2CManager.hpp>

#include <peripherals/I2CSettings.hpp>
#include <peripherals/Peripheral.hpp>
#include <utility>

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::mqtt;
using namespace cornucopia::ugly_duckling::peripherals;

namespace cornucopia::ugly_duckling::peripherals::environment {

LOGGING_TAG(ENV, "env")

class EnvironmentSensor {
public:
    virtual ~EnvironmentSensor() = default;

    virtual double getTemperature() = 0;
    virtual double getMoisture() = 0;
};

}    // namespace cornucopia::ugly_duckling::peripherals::environment
