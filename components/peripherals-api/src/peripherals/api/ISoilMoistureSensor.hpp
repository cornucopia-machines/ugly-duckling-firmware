#pragma once

#include "IPeripheral.hpp"
#include "Units.hpp"

namespace cornucopia::ugly_duckling::peripherals::api {

struct ISoilMoistureSensor : virtual IPeripheral {
    virtual Percent getMoisture() = 0;    // Returns a raw moisture percentage reading
};

}    // namespace cornucopia::ugly_duckling::peripherals::api
