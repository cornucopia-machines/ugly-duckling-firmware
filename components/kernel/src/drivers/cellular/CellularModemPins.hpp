#pragma once

#include <Pin.hpp>

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

/**
 * @brief How a cellular modem connector is wired, from the MCU's point of view.
 */
struct CellularModemPins {
    // MCU TX -> modem RX
    InternalPinPtr tx;
    // Modem TX -> MCU RX
    InternalPinPtr rx;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
