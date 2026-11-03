#pragma once

#include "Drv8833Driver.hpp"

namespace cornucopia::ugly_duckling::kernel::drivers {

/**
 * @brief Texas Instruments DRV8847 dual motor driver.
 *
 * In its 4-pin interface (MODE tied low) each bridge follows the same IN1/IN2 truth table as the
 * DRV8833: one input high drives, both low coasts, both high brakes (slow decay). nSLEEP must be
 * high to run; boards tie it to the load enable.
 *
 * https://www.ti.com/lit/gpn/DRV8847
 */
using Drv8847Driver = Drv8833Driver;

}    // namespace cornucopia::ugly_duckling::kernel::drivers
