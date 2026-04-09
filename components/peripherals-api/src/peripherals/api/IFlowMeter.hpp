#pragma once

#include "IPeripheral.hpp"
#include "Units.hpp"

namespace cornucopia::ugly_duckling::peripherals::api {

struct IFlowMeter : virtual IPeripheral {
    virtual Liters getVolume() = 0;
};

}    // namespace cornucopia::ugly_duckling::peripherals::api
