#pragma once

#include "IPeripheral.hpp"
#include "Units.hpp"

namespace cornucopia::ugly_duckling::peripherals::api {

struct ITemperatureSensor : virtual IPeripheral {
    virtual Celsius getTemperature() = 0;  // Returns a raw temperature reading
};

}    // namespace cornucopia::ugly_duckling::peripherals::api
