#pragma once

#include <Log.hpp>
#include <drivers/cellular/AtResponse.hpp>
#include <drivers/cellular/AtSocket.hpp>
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
using SocketEventHandler = std::function<void(SocketEventType type)>;

/**
 * @brief What one read from the module's receive buffer returned.
 */
struct SocketReceive {
    size_t length;
    // Whether the module still holds more data after this read
    bool more;
};

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
     *
     * @return the serving cell it read, if any
     */
    virtual std::optional<ServingCell> logRadioStatus() = 0;

    /**
     * @brief The serving cell's radio state, without logging it.
     */
    virtual std::optional<ServingCell> queryServingCell() = 0;

    /**
     * @brief The IP address of the default PDP context, if the network has assigned one.
     */
    virtual std::optional<std::string> queryIpAddress() = 0;

    /**
     * @brief Gets the module ready to open connections once it has an IP address, e.g. makes sure
     * there is a DNS server to resolve hostnames with.
     */
    virtual bool prepareNetwork() = 0;

    /**
     * @brief Asks an NTP server for the time, through the module's own NTP client.
     *
     * @return the current time in UTC, or nullopt if the query failed
     */
    virtual std::optional<time_t> queryNtpTime(const std::string& server) = 0;

    /**
     * @brief Opens the TCP connection, waiting until it is either established or has failed.
     *
     * The module supports one connection at a time: the one MQTT runs over. Received data stays
     * in the module until read with receive(); a SocketEventType::DataAvailable event says when
     * there is some.
     *
     * @param host an IP address or a hostname, which the module resolves itself
     */
    virtual bool openSocket(const std::string& host, int port) = 0;

    virtual void closeSocket() = 0;

    /**
     * @brief Sends at most getMaxSendSize() bytes.
     */
    virtual bool send(const uint8_t* data, size_t length) = 0;

    virtual size_t getMaxSendSize() const = 0;

    /**
     * @brief Reads at most getMaxReceiveSize() bytes of what the module has received.
     *
     * @return how much was read, 0 if nothing was waiting; nullopt if the module failed to answer
     */
    virtual std::optional<SocketReceive> receive(uint8_t* buffer, size_t length) = 0;

    virtual size_t getMaxReceiveSize() const = 0;

    /**
     * @brief Registers the handler for socket events (data waiting, connection closed).
     *
     * Called from the UART's receive task: the handler must not send commands to the module.
     */
    virtual void onSocketEvent(SocketEventHandler handler) = 0;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
