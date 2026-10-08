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
 * @brief What one read from the module's receive buffer returned.
 */
struct SocketReceive {
    size_t length;
    // Whether the module still holds more data after this read
    bool more;
    // How much data the module still holds, if it says
    std::optional<size_t> remaining;
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
     * @brief Finds the module after boot, at whichever UART rate it is on, and switches it to the
     * fastest rate the link takes.
     *
     * Call before anything else; afterwards, wake() is enough.
     */
    virtual bool start() = 0;

    /**
     * @brief Makes sure the module is awake and answering commands.
     */
    virtual bool wake() = 0;

    /**
     * @brief Applies the settings the module does not keep across its own restarts.
     *
     * Called once the module answers after boot; safe to repeat.
     *
     * @param allowSleep whether the module may sleep between commands; false for a boot that
     * downloads a firmware update, since the module doesn't announce received data promptly
     * while sleep is enabled, and its receive buffer overflows
     */
    virtual bool configure(bool allowSleep) = 0;

    /**
     * @brief Keeps the module reachable while it sleeps: PSM off, and eDRX with the given cycle,
     * or off for plain DRX paging.
     *
     * The module keeps these across restarts, and the network learns about every change, so
     * only what differs gets written. The network decides what it grants, see queryEdrx().
     */
    virtual bool configurePowerSaving(std::optional<milliseconds> edrxCycle) = 0;

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
     * @brief The eDRX parameters the network granted on the current cell.
     */
    virtual std::optional<EdrxParameters> queryEdrx() = 0;

    /**
     * @brief The paging cycle the module uses while RRC idle and not in eDRX: the cell's default
     * paging cycle (1.28 to 10.24 s in NB-IoT).
     *
     * @return nullopt while RRC connected, when the module only reports connected mode DRX
     */
    virtual std::optional<milliseconds> queryIdlePagingCycle() = 0;

    /**
     * @brief Whether the module has an RRC connection to the cell (true) or is idle (false).
     *
     * The module only updates this on radio events, so it can be a little out of date.
     */
    virtual std::optional<bool> queryRrcConnected() = 0;

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
     * there is some. Unless pushesSocketData(): then it arrives in SocketEventType::DataPushed
     * events instead.
     *
     * @param host an IP address or a hostname, which the module resolves itself
     */
    virtual bool openSocket(const std::string& host, int port) = 0;

    virtual void closeSocket() = 0;

    /**
     * @brief Whether the module hands over received data as soon as it arrives (direct push
     * mode), instead of keeping it until receive() reads it.
     *
     * The module's receive buffer only holds about 2 KB, and while the ESP32 writes flash it
     * can't read: every flash erase or write stops everything not in IRAM. During update
     * downloads the buffer overflowed. Pushed data waits in the ESP32's UART buffer instead,
     * which the UART interrupt keeps filling (CONFIG_UART_ISR_IN_IRAM). There's no flow control,
     * though, and commands from elsewhere wait until the connection closes, so it's only for
     * downloads.
     */
    virtual bool pushesSocketData() const = 0;

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
     * @brief How many times the UART lost or garbled data from the module since boot: overflows
     * and framing or parity errors.
     */
    virtual uint32_t getUartErrorCount() const = 0;

    /**
     * @brief Registers the handler for socket events (data waiting or pushed, connection closed).
     *
     * Called from the UART's receive task: the handler must not send commands to the module. The
     * event's data is only valid during the call.
     */
    virtual void onSocketEvent(SocketEventHandler handler) = 0;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
