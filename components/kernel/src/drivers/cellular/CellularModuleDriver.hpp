#pragma once

#include <Log.hpp>
#include <drivers/cellular/AtResponse.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

using namespace std::chrono;

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

LOGGING_TAG(CELLULAR, "cellular")

using UrcHandler = std::function<void(std::string_view line)>;

/**
 * @brief The chipset-specific half of the cellular stack: everything that differs between modems.
 *
 * CellularDriver owns the UART and the lifecycle and talks to the modem only through this
 * interface, so supporting another modem means adding another implementation (see
 * docs/specs/NB-IoT.md, "Leave room for other chipsets").
 */
class CellularModuleDriver {
public:
    virtual ~CellularModuleDriver() = default;

    virtual const char* getName() const = 0;

    /**
     * @brief Makes sure the module is awake and answering commands.
     */
    virtual bool wake() = 0;

    /**
     * @brief Applies the settings the module does not keep across its own restarts.
     *
     * Called once the module answers after boot; safe to repeat.
     */
    virtual bool configure() = 0;

    /**
     * @brief Sends an AT command, waking the module first, and waits for its final result code.
     *
     * @param command the command without the trailing CR, e.g. "AT+CSQ"
     */
    virtual AtResponse command(const std::string& command, milliseconds timeout) = 0;

    /**
     * @brief Registers the handler for unsolicited result codes, called with one line at a time.
     *
     * A URC that arrives while a command is in flight ends up among that command's response
     * lines instead.
     */
    virtual void onUrc(UrcHandler handler) = 0;

    /**
     * @brief Logs the module's identity and SIM state, for bring-up.
     */
    virtual void logStatus() = 0;

    /**
     * @brief Logs signal strength and serving cell, decoded.
     */
    virtual void logRadioStatus() = 0;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
