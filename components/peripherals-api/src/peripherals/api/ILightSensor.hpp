#pragma once

#include "IPeripheral.hpp"
#include "Units.hpp"

namespace cornucopia::ugly_duckling::peripherals::api {

struct ILightSensor : virtual IPeripheral {
    virtual Lux getLightLevel() = 0;
};

}    // namespace cornucopia::ugly_duckling::peripherals::api
