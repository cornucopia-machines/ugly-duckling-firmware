#pragma once

#include <Log.hpp>
#include <drivers/cellular/AtResponse.hpp>
#include <drivers/cellular/AtSocket.hpp>
#include <drivers/cellular/Edrx.hpp>
#include <drivers/cellular/RadioStatus.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

using namespace std::chrono;

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

LOGGING_TAG(CELLULAR, "cellular")

using UrcHandler = std::function<void(std::string_view line)>;
using SocketEventHandler = std::function<void(const SocketEvent& event)>;

/**
 * @brief Identifies a connection opened with CellularModuleDriver::openSocket().
 *
 * Assigned by the driver and never reused, unlike the modem's own socket handles: a handle freed
 * by a modem restart can come back for another connection, which a late close of the old one
 * mustn't take down.
 */
using SocketId = uint32_t;

/**
 * @brief What one read from the modem's socket returned.
 */
struct SocketReceive {
    size_t length;
    // Whether the modem may hold more data after this read
    bool more;
    // Whether the peer has closed the connection, and everything it sent has been read
    bool closed;
};

/**
 * @brief The chipset-specific half of the cellular stack: everything that differs between modems.
 *
 * CellularDriver owns the lifecycle and talks to the modem only through this interface, so
 * supporting another modem means adding another implementation.
 */
class CellularModuleDriver {
public:
    virtual ~CellularModuleDriver() = default;

    virtual const char* getName() const = 0;

    /**
     * @brief Finds the modem after boot, at whichever UART rate it is on, and switches it to the
     * fastest rate the link takes. Resets the modem if it doesn't answer.
     *
     * Call before anything else.
     */
    virtual bool start() = 0;

    /**
     * @brief Applies the settings the modem doesn't keep across its own restarts, and turns the
     * radio on. Safe to repeat; the driver repeats it by itself after the modem restarts.
     *
     * @param allowSleep whether the modem's UART may power down between commands; false for a
     * boot that downloads a firmware update
     */
    virtual bool configure(bool allowSleep) = 0;

    /**
     * @brief Keeps the modem reachable while it sleeps: PSM off, and eDRX with the given cycle,
     * or off for plain DRX paging.
     *
     * The network decides what it grants, see queryEdrx().
     */
    virtual bool configurePowerSaving(std::optional<milliseconds> edrxCycle) = 0;

    /**
     * @brief Sends an AT command, waking the modem's UART first, and waits for its final result
     * code.
     *
     * @param command the command without the trailing CR, e.g. "AT+CEREG?"
     */
    virtual AtResponse command(const std::string& command, milliseconds timeout) = 0;

    /**
     * @brief Registers the handler for unsolicited result codes, called with one line at a time
     * from the UART's reader task: it must not send commands.
     */
    virtual void onUrc(UrcHandler handler) = 0;

    /**
     * @brief Logs the modem's identity and SIM state, for bring-up.
     */
    virtual void logStatus() = 0;

    /**
     * @brief Logs the serving cell and its signal, decoded.
     *
     * @return the serving cell it read, if any
     */
    virtual std::optional<ServingCell> logRadioStatus() = 0;

    /**
     * @brief The serving cell's radio state, without logging it.
     */
    virtual std::optional<ServingCell> queryServingCell() = 0;

    /**
     * @brief The eDRX parameters the network granted on the current cell.
     */
    virtual std::optional<EdrxParameters> queryEdrx() = 0;

    /**
     * @brief Whether the modem has an RRC connection to the cell (true) or is idle (false).
     */
    virtual std::optional<bool> queryRrcConnected() = 0;

    /**
     * @brief Tells the network that no more data is expected, so it releases the RRC connection
     * now instead of when its inactivity timer runs out (release assistance).
     *
     * @return false if the connection isn't open, another one is (whose data may still be on its
     * way), or the modem refused
     */
    virtual bool releaseRrc(SocketId socket) = 0;

    virtual std::optional<std::string> queryIpAddress() = 0;

    /**
     * @brief Gets the modem ready to open connections once it has an IP address.
     */
    virtual bool prepareNetwork() = 0;

    /**
     * @brief Asks an NTP server for the time.
     *
     * @return the current time in UTC, or nullopt if the query failed
     */
    virtual std::optional<time_t> queryNtpTime(const std::string& server) = 0;

    /**
     * @brief Opens a TCP connection, waiting until it is either established or has failed.
     *
     * Several can be open at once: MQTT keeps its connection while a firmware update downloads.
     * Received data stays in the modem until read with receive(); a SocketEventType::DataAvailable
     * event says when there is some.
     *
     * @param host an IP address or a hostname, which the modem resolves itself
     * @param handler called with this connection's events, from the UART's reader task: it must
     * not send commands to the modem. Called from the start, as data can arrive as soon as the
     * connection is up; never again once closeSocket() returns
     * @return the connection, or nullopt if it couldn't be opened
     */
    virtual std::optional<SocketId> openSocket(const std::string& host, int port, SocketEventHandler handler) = 0;

    /**
     * @brief Closes the connection; does nothing if the modem has dropped it already.
     */
    virtual void closeSocket(SocketId socket) = 0;

    /**
     * @brief Sends at most getMaxSendSize() bytes.
     */
    virtual bool send(SocketId socket, const uint8_t* data, size_t length) = 0;

    virtual size_t getMaxSendSize() const = 0;

    /**
     * @brief Reads at most getMaxReceiveSize() bytes of what the modem has received.
     *
     * @return how much was read, 0 if nothing was waiting; nullopt if the modem failed to answer
     */
    virtual std::optional<SocketReceive> receive(SocketId socket, uint8_t* buffer, size_t length) = 0;

    virtual size_t getMaxReceiveSize() const = 0;

    /**
     * @brief How many times the UART lost or garbled data from the modem since boot: overflows
     * and framing or parity errors.
     */
    virtual uint32_t getUartErrorCount() const = 0;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
