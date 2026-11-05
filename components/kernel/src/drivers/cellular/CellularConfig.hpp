#pragma once

#include <config/Configuration.hpp>

#include <chrono>
#include <string>

using namespace std::chrono;
using namespace std::chrono_literals;

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

/**
 * @brief The "cellular" section of network-config: which radio to use, and how the modem trades
 * responsiveness for power (docs/Connectivity.md, "Cellular").
 */
struct CellularConfig : ConfigurationSection {
    /**
     * @brief The radio access technologies the modem may use, in order of preference: "lte-m" and
     * "nb-iot". Empty (the default) means both, LTE-M first.
     *
     * The modem keeps the setting in flash, and changing it costs a detach and a fresh network
     * search, so it's only written when it differs. Unknown names are ignored.
     */
    ArrayProperty<std::string> rats { this, "rats" };

    /**
     * @brief How often the modem listens for paging between transfers (eDRX), which bounds how
     * long a command from the server takes to arrive.
     *
     * 0 (the default) leaves eDRX off: the modem listens on the paging cycle the cell broadcasts
     * (1.28 to 10.24 s). Otherwise one of the eDRX cycles: on LTE-M 5120, 10240, 20480, 40960,
     * 61440, 81920, 102400, 122880, 143360 ms and up, on NB-IoT 20480, 40960, 81920 ms and up
     * (LTE_M_EDRX_CYCLES, NB_IOT_EDRX_CYCLES). It's requested on each RAT that has the cycle.
     * Off by default because commands should arrive within seconds, and the network has the final
     * say anyway; the log shows what it granted.
     */
    Property<milliseconds> edrxCycle { this, "edrxCycle", 0ms };
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
