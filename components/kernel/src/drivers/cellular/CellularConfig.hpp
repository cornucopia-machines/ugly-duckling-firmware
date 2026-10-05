#pragma once

#include <config/Configuration.hpp>

#include <chrono>

using namespace std::chrono;
using namespace std::chrono_literals;

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

/**
 * @brief The "cellular" section of network-config: how the modem trades responsiveness for power
 * (docs/specs/NB-IoT.md, "Stage 4").
 */
struct CellularConfig : ConfigurationSection {
    /**
     * @brief How often the modem listens for paging between transfers (eDRX), which bounds how
     * long a command from the server takes to arrive.
     *
     * 0 (the default) leaves eDRX off: the modem listens on the paging cycle the cell broadcasts
     * (1.28 to 10.24 s). Otherwise one of the cycles NB-IoT has: 20480, 40960, 81920, 163840,
     * 327680, 655360, 1310720, 2621440, 5242880 or 10485760 ms (EDRX_CYCLES). Off by default
     * because the network has the final say, and the one we use (Telekom, roaming on 1NCE)
     * doesn't grant eDRX; the log shows what the network granted, and the paging cycle.
     */
    Property<milliseconds> edrxCycle { this, "edrxCycle", 0ms };
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
