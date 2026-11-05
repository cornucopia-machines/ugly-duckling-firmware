#pragma once

#include <Pin.hpp>

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

/**
 * @brief How a cellular modem connector is wired, from the MCU's point of view.
 *
 * The signal names follow the MCU's side: rts is the MCU's RTS output, which goes to the modem's
 * CTS input, and so on.
 */
struct CellularModemPins {
    // MCU TX -> modem RX
    InternalPinPtr tx;
    // Modem TX -> MCU RX
    InternalPinPtr rx;
    // MCU RTS -> modem CTS: the modem only sends while this is low
    InternalPinPtr rts;
    // Modem RTS -> MCU CTS: we only send while this is low. Pulled up on our side, as Serial Modem
    // asks of the host: while the modem's UART is off, its RTS doesn't drive the line
    InternalPinPtr cts;
    // MCU -> modem DTR, active low: the modem keeps its UART on while it is low. Pulled up on the
    // modem board, so it reads deasserted while the MCU doesn't drive it
    InternalPinPtr dtr;
    // Modem -> MCU ring indicator, active low: the modem has something to send while its UART is
    // off. Push-pull, but floats while the modem is in reset or booting
    InternalPinPtr ri;
    // MCU -> modem nRESET, active low. Open-drain only: pulled low or released, never driven high
    // (the nRF9151 pulls it up to an internal 2.2 V, and forbids an external pull-up)
    InternalPinPtr reset;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
