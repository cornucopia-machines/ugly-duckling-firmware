#pragma once

#include <Log.hpp>
#include <PowerManager.hpp>
#include <Result.hpp>
#include <Task.hpp>
#include <drivers/cellular/AtChannel.hpp>
#include <drivers/cellular/AtResponse.hpp>
#include <drivers/cellular/AtSocket.hpp>
#include <drivers/cellular/CellularModemPins.hpp>
#include <drivers/cellular/CellularModuleDriver.hpp>
#include <drivers/cellular/Edrx.hpp>
#include <drivers/cellular/NetworkTime.hpp>
#include <drivers/cellular/RadioStatus.hpp>
#include <drivers/cellular/Rat.hpp>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>    // NOLINT(misc-header-include-cycle)
#include <freertos/queue.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono;
using namespace std::chrono_literals;

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

/**
 * @brief Nordic nRF9151 running Serial Modem 2.0 (LTE-M and NB-IoT), on the Desert Lark MK2
 * daughter board.
 *
 * The UART has hardware flow control, and DTR/RI power management: with DTR deasserted (high),
 * Serial Modem turns its UART off, and asserts RI when it has something to say. So DTR is held
 * low, with the ESP32 kept out of light sleep, only while commands run and for UART_IDLE_TIMEOUT
 * after the last byte either way; RI wakes us for everything the modem sends in between (see
 * docs/Connectivity.md, "Cellular").
 *
 * Socket data goes over the UART as hex (AT#XSEND and AT#XRECV in hex string mode): every
 * command and response stays a text line. That costs twice the bytes, which at 921600 baud still
 * leaves the UART faster than LTE-M.
 */
class Nrf9151Driver final : public CellularModuleDriver {
public:
    /**
     * @param rats the radio access technologies to use, in order of preference
     */
    static Result<std::shared_ptr<Nrf9151Driver>> create(uart_port_t port, const CellularModemPins& pins, std::vector<Rat> rats) {
        auto channel = TRY(AtChannel::create(port, pins, DEFAULT_BAUD_RATE, &Nrf9151Driver::isUrc));
        // Never destroyed, not even when setting up the pins fails: the UART's reader task already
        // calls into it
        auto* driver = new Nrf9151Driver(pins, std::move(channel), std::move(rats));
        TRY(driver->setUpPins());
        return std::shared_ptr<Nrf9151Driver>(driver);
    }

    Nrf9151Driver(const Nrf9151Driver&) = delete;
    Nrf9151Driver& operator=(const Nrf9151Driver&) = delete;

    const char* getName() const override {
        return "nRF9151";
    }

    /**
     * Serial Modem has no autobaud, and starts at DEFAULT_BAUD_RATE whenever the nRF9151 boots
     * (AT+IPR isn't saved). An ESP32 restart doesn't restart the modem, though, so it can still
     * be on the fast rate from the last boot.
     */
    bool start() override {
        std::scoped_lock lock(commandMutex);
        return startLocked();
    }

    bool configure(bool allowSleep) override {
        std::scoped_lock lock(commandMutex);
        return configureLocked(allowSleep);
    }

    bool configurePowerSaving(std::optional<milliseconds> edrxCycle) override {
        std::scoped_lock lock(commandMutex);
        return configurePowerSavingLocked(edrxCycle);
    }

    AtResponse command(const std::string& command, milliseconds timeout) override {
        std::scoped_lock lock(commandMutex);
        recoverIfRestarted();
        return at(command, timeout);
    }

    void onUrc(UrcHandler handler) override {
        std::scoped_lock lock(handlerMutex);
        urcHandler = std::move(handler);
    }

    void logStatus() override {
        std::scoped_lock lock(commandMutex);
        recoverIfRestarted();
        // Serial Modem and NCS version, then the modem firmware
        logResponse("AT#XSMVER");
        logResponse("AT+CGMR");
        // IMEI: the device identity the network (and the SIM provider's IMEI lock) sees
        logResponse("AT+CGSN=1");
        // SIM: which one it is, and the subscription on it. Right after the radio goes on, the
        // SIM takes a moment to answer again (CME ERROR 0 until then)
        logSimResponse("AT%XICCID");
        logSimResponse("AT+CIMI");
        // Which RATs are enabled, and which is preferred
        logResponse("AT%XSYSTEMMODE?");
        // Bands locked out: permanent, runtime and erase-protected masks first (bit strings, band
        // 1 last), all "" without a lock; mfw 2.0.4 answers with three more, undocumented. A permanent lock survives in the modem's flash, and we
        // never set one, so one left from earlier testing would hide every cell outside it
        logResponse("AT%XBANDLOCK?");
    }

    std::optional<ServingCell> logRadioStatus() override {
        std::scoped_lock lock(commandMutex);
        recoverIfRestarted();
        auto cell = queryServingCellLocked();
        LOGTI(CELLULAR, "Radio: %s", cell ? describe(*cell).c_str() : "no %XMONITOR answer");
        return cell;
    }

    std::optional<ServingCell> queryServingCell() override {
        std::scoped_lock lock(commandMutex);
        recoverIfRestarted();
        return queryServingCellLocked();
    }

    std::optional<EdrxParameters> queryEdrx() override {
        return findDecoded(command("AT+CEDRXRDP", DEFAULT_TIMEOUT), parseCedrxrdp);
    }

    std::optional<bool> queryRrcConnected() override {
        return findDecoded(command("AT+CSCON?", DEFAULT_TIMEOUT), parseCsconRead);
    }

    bool releaseRrc(SocketId socket) override {
        std::scoped_lock lock(commandMutex);
        recoverIfRestarted();
        std::optional<int> handle;
        {
            std::scoped_lock socketLock(socketMutex);
            // The release ends the RRC connection for every socket, and another one may still be
            // waiting for data (MQTT while a firmware update downloads, or the other way round)
            if (sockets.size() != 1) {
                return false;
            }
            handle = findHandleLocked(socket);
        }
        if (!handle) {
            return false;
        }
        // RAI_NO_DATA: nothing more to send, so the modem can leave connected mode right away
        auto response = at("AT#XSOCKETOPT=" + std::to_string(*handle) + ",1,61,1", DEFAULT_TIMEOUT);
        if (!response.ok()) {
            LOGTD(CELLULAR, "Release assistance failed: %s %s", toString(response.result), response.error.c_str());
        }
        return response.ok();
    }

    std::optional<std::string> queryIpAddress() override {
        std::scoped_lock lock(commandMutex);
        recoverIfRestarted();
        // Context 0 is the default bearer the modem sets up when it attaches
        auto response = at("AT+CGPADDR=0", DEFAULT_TIMEOUT);
        auto fields = response.ok() ? findFields(response, "+CGPADDR:") : std::nullopt;
        if (!fields || fields->size() < 2) {
            return std::nullopt;
        }
        auto address = (*fields)[1].asString();
        return address && !address->empty() ? std::optional<std::string>(*address) : std::nullopt;
    }

    bool prepareNetwork() override {
        std::scoped_lock lock(commandMutex);
        recoverIfRestarted();
        // The APN, the DNS servers and the MTU the network handed out: hostnames resolve through
        // those DNS servers
        logResponse("AT+CGCONTRDP=0");
        return true;
    }

    /**
     * Serial Modem has no NTP command, so this asks over a UDP socket (SNTP).
     */
    std::optional<time_t> queryNtpTime(const std::string& server) override {
        std::scoped_lock lock(commandMutex);
        recoverIfRestarted();
        auto open = at("AT#XSOCKET=1,2,0", DEFAULT_TIMEOUT);
        auto handle = open.ok() ? findDecoded(open, parseXsocket) : std::nullopt;
        if (!handle) {
            LOGTW(CELLULAR, "Could not open a socket for NTP: %s %s", toString(open.result), open.error.c_str());
            return std::nullopt;
        }
        auto id = std::to_string(*handle);
        auto request = makeSntpRequest();
        std::optional<time_t> time;
        // Resolving the server's name is part of the send
        auto sent = at("AT#XSENDTO=" + id + ",1,0,\"" + server + "\",123,\"" + toHex(request.data(), request.size()) + "\"", CONNECT_TIMEOUT);
        if (!sent.ok()) {
            LOGTW(CELLULAR, "Could not send the NTP request to %s: %s %s", server.c_str(), toString(sent.result), sent.error.c_str());
        } else {
            // Blocks in the modem for up to NTP_WAIT
            auto received = at("AT#XRECVFROM=" + id + ",1,0," + std::to_string(duration_cast<seconds>(NTP_WAIT).count()), NTP_WAIT + DEFAULT_TIMEOUT);
            // Room for extension fields, which the time doesn't need
            std::array<uint8_t, 128> reply {};
            auto length = received.ok() ? readHexData(received, "#XRECVFROM:", *handle, reply.data(), reply.size()) : std::nullopt;
            time = length ? parseSntpResponse(reply.data(), *length) : std::nullopt;
            if (!time) {
                LOGTW(CELLULAR, "No usable NTP reply from %s: %s %s", server.c_str(), toString(received.result), received.error.c_str());
            }
        }
        at("AT#XCLOSE=" + id, DEFAULT_TIMEOUT);
        return time;
    }

    std::optional<time_t> queryClock() override {
        std::scoped_lock lock(commandMutex);
        recoverIfRestarted();
        // ERROR until the network sent its time, or AT+CCLK set it
        auto response = at("AT+CCLK?", DEFAULT_TIMEOUT);
        return response.ok() ? findDecoded(response, parseCclk) : std::nullopt;
    }

    bool setClock(time_t utcTime) override {
        auto time = formatCclk(utcTime);
        if (!time) {
            return false;
        }
        std::scoped_lock lock(commandMutex);
        recoverIfRestarted();
        return apply("AT+CCLK=\"" + *time + "\"");
    }

    std::optional<SocketId> openSocket(const std::string& host, int port, SocketEventHandler handler) override {
        std::scoped_lock lock(commandMutex);
        recoverIfRestarted();
        auto open = at("AT#XSOCKET=1,1,0", DEFAULT_TIMEOUT);
        auto handle = open.ok() ? findDecoded(open, parseXsocket) : std::nullopt;
        if (!handle) {
            LOGTW(CELLULAR, "Could not open a socket: %s %s", toString(open.result), open.error.c_str());
            return std::nullopt;
        }
        // Announcements can come as soon as it connects
        SocketId socket;
        {
            std::scoped_lock socketLock(socketMutex);
            socket = nextSocketId++;
            sockets.push_back(OpenSocket { .id = socket, .handle = *handle, .handler = std::move(handler) });
        }
        auto connect = "AT#XCONNECT=" + std::to_string(*handle) + ",\"" + host + "\"," + std::to_string(port);
        auto response = at(connect, CONNECT_TIMEOUT);
        bool connected = response.ok() && std::ranges::any_of(response.lines, [&](const std::string& line) {
            return parseXconnect(line, *handle);
        });
        if (!connected) {
            LOGTW(CELLULAR, "Could not connect to %s:%d: %s %s", host.c_str(), port, toString(response.result),
                response.lines.empty() ? response.error.c_str() : response.lines.front().c_str());
            closeSocketLocked(socket);
            return std::nullopt;
        }
        LOGTD(CELLULAR, "Connected to %s:%d on socket %d", host.c_str(), port, *handle);
        return socket;
    }

    void closeSocket(SocketId socket) override {
        std::scoped_lock lock(commandMutex);
        closeSocketLocked(socket);
    }

    bool send(SocketId socket, const uint8_t* data, size_t length) override {
        std::scoped_lock lock(commandMutex);
        recoverIfRestarted();
        auto handle = findHandle(socket);
        if (!handle) {
            return false;
        }
        auto response = at("AT#XSEND=" + std::to_string(*handle) + ",1,0,\"" + toHex(data, length) + "\"", SEND_TIMEOUT);
        auto sent = response.ok() ? findDecoded(response, [&](std::string_view line) {
            return parseXsend(line, "#XSEND:", *handle);
        })
                                  : std::nullopt;
        if (sent != length) {
            LOGTW(CELLULAR, "Sending %zu bytes failed: %s %s", length, toString(response.result),
                sent ? (std::to_string(*sent) + " sent").c_str() : response.error.c_str());
            return false;
        }
        return true;
    }

    size_t getMaxSendSize() const override {
        // The most AT#XSEND takes in hex string mode
        return 1400;
    }

    std::optional<SocketReceive> receive(SocketId socket, uint8_t* buffer, size_t length) override {
        std::scoped_lock lock(commandMutex);
        recoverIfRestarted();
        auto found = findHandle(socket);
        if (!found) {
            // Gone with a modem restart
            return SocketReceive { .length = 0, .more = false, .closed = true };
        }
        int handle = *found;
        length = std::min(length, getMaxReceiveSize());
        // Hex string mode, non-blocking (flag 64)
        auto read = "AT#XRECV=" + std::to_string(handle) + ",1,64,0," + std::to_string(length);
        auto response = at(read, RECEIVE_TIMEOUT);
        switch (response.result) {
            case AtResult::Timeout:
                return std::nullopt;
            case AtResult::Error:
            case AtResult::CmeError:
                // Nothing to read on an open connection (EAGAIN) answers ERROR
                return SocketReceive { .length = 0, .more = false, .closed = false };
            case AtResult::Ok:
                break;
        }
        bool hasData = std::ranges::any_of(response.lines, [&](const std::string& line) {
            return parseXrecvHeader(line, "#XRECV:", handle).has_value();
        });
        if (!hasData) {
            // A bare OK: the peer closed the connection, and everything it sent has been read
            return SocketReceive { .length = 0, .more = false, .closed = true };
        }
        auto received = readHexData(response, "#XRECV:", handle, buffer, length);
        if (!received) {
            // The data was taken out of the modem either way, so the stream has a gap now
            LOGTW(CELLULAR, "%s: malformed response, %zu lines", read.c_str(), response.lines.size());
            return std::nullopt;
        }
        return SocketReceive { .length = *received, .more = *received == length, .closed = false };
    }

    size_t getMaxReceiveSize() const override {
        // The most AT#XRECV returns at once
        return 2048;
    }

    uint32_t getUartErrorCount() const override {
        return channel->getErrorCount();
    }

private:
    bool startLocked() {
        bool found = findAndSpeedUp();
        if (!found) {
            // Nobody to keep the UART on for: let the ESP32 sleep
            std::scoped_lock powerLock(powerMutex);
            sleepAllowed = true;
            return false;
        }
        // A "Ready" seen while starting is the boot we're starting from
        restarted = false;
        started = true;
        return true;
    }

    bool findAndSpeedUp() {
        UartUse uart(*this);
        auto rate = findBaudRate();
        if (!rate) {
            if (!mayReset()) {
                return false;
            }
            LOGTW(CELLULAR, "%s is not answering, resetting it", getName());
            reset();
            rate = findBaudRate();
            if (!rate) {
                return false;
            }
        }
        if (*rate != FAST_BAUD_RATE) {
            switchBaudRate();
            if (!findBaudRate()) {
                return false;
            }
        }
        return true;
    }

    bool configureLocked(bool allowSleep) {
        this->allowSleep = allowSleep;
        if (!allowSleep) {
            LOGTI(CELLULAR, "Keeping the modem's UART on for this boot");
        }

        // None of these is saved by the modem, and none is ever wrong to repeat
        static constexpr std::array SETTINGS {
            // Numeric "+CME ERROR: <err>" instead of a bare ERROR
            "AT+CMEE=1",
            // Mode 5 carries the EMM reject cause, which tells "no coverage" apart from
            // "subscription refused", and the PSM timers
            "AT+CEREG=5",
            // Report the RRC connection going up and down (+CSCON), for the debug console and
            // for telemetry on how long the radio stays connected
            "AT+CSCON=1",
            // Report network searches, resets and other modem events (%MDMEV), for the log
            "AT%MDMEV=1",
            // Report network time (NITZ) as %XTIME, in UTC
            "AT%XTIME=1",
            // Announce received data and closed connections on every socket (#XAPOLL)
            "AT#XAPOLL=,1,1",
        };
        bool success = true;
        for (const char* setting : SETTINGS) {
            success = apply(setting) && success;
        }
        // Sockets left open by an earlier boot, or by this one before the modem restarted: all of
        // them are ours
        at("AT#XCLOSE", DEFAULT_TIMEOUT);
        dropAllSockets();

        // Before the radio goes on: the system mode only changes with it off
        success = ensureSystemMode() && success;
        success = ensureReleaseAssistance() && success;
        success = ensureFullFunctionality() && success;

        configured = true;
        {
            std::scoped_lock powerLock(powerMutex);
            sleepAllowed = allowSleep;
        }
        armRingIndicator();
        return success;
    }

    bool configurePowerSavingLocked(std::optional<milliseconds> edrxCycle) {
        this->edrxCycle = edrxCycle;
        powerSavingConfigured = true;
        // In PSM the modem is unreachable until its own next wake; eDRX, or plain DRX paging,
        // keeps it reachable with a bounded delay instead
        bool success = apply("AT+CPSMS=0");
        if (!edrxCycle) {
            // Turns eDRX off on every RAT, and discards the requested cycles
            return apply("AT+CEDRXS=0") && success;
        }
        for (auto rat : rats) {
            auto code = encodeEdrxCycle(rat, *edrxCycle);
            if (!code) {
                LOGTW(CELLULAR, "%s has no %lld ms eDRX cycle, turning eDRX off on it", toString(rat),
                    static_cast<long long>(edrxCycle->count()));
                success = apply("AT+CEDRXS=0," + std::to_string(edrxAccessTechnology(rat))) && success;
                continue;
            }
            // Mode 2 also reports what the network grants as +CEDRXP
            success = apply("AT+CEDRXS=2," + std::to_string(edrxAccessTechnology(rat)) + ",\"" + *code + "\"") && success;
        }
        return success;
    }

    /**
     * @brief Sends a command, with the UART in use and commandMutex held. Unlike command(), it
     * doesn't set the modem up again after a restart, so recovery can use it.
     */
    AtResponse at(const std::string& command, milliseconds timeout) {
        UartUse uart(*this);
        auto startedAt = steady_clock::now();
        auto response = channel->command(command, timeout);
        auto took = steady_clock::now() - startedAt;
        if (took >= SLOW_COMMAND) {
            LOGTD(CELLULAR, "%s took %lld ms", command.c_str(),
                static_cast<long long>(duration_cast<milliseconds>(took).count()));
        }
        if (response.result == AtResult::Timeout) {
            LOGTD(CELLULAR, "%s: no answer in %lld ms", command.c_str(), static_cast<long long>(timeout.count()));
            resetIfUnresponsive();
        } else {
            unansweredCommands = 0;
        }
        return response;
    }

    std::optional<ServingCell> queryServingCellLocked() {
        auto cell = findDecoded(at("AT%XMONITOR", DEFAULT_TIMEOUT), parseXmonitor);
        if (cell && cell->isCamped()) {
            for (const auto& line : at("AT%CONEVAL", DEFAULT_TIMEOUT).lines) {
                addConeval(*cell, line);
            }
        }
        return cell;
    }

    void closeSocketLocked(SocketId socket) {
        std::optional<int> handle;
        {
            std::scoped_lock socketLock(socketMutex);
            auto it = std::ranges::find(sockets, socket, &OpenSocket::id);
            if (it != sockets.end()) {
                handle = it->handle;
                sockets.erase(it);
            }
        }
        if (handle) {
            // Answers ERROR when the modem has dropped the socket already, which is fine
            at("AT#XCLOSE=" + std::to_string(*handle), DEFAULT_TIMEOUT);
        }
    }

    std::optional<int> findHandle(SocketId socket) {
        std::scoped_lock socketLock(socketMutex);
        return findHandleLocked(socket);
    }

    std::optional<int> findHandleLocked(SocketId socket) const {
        auto it = std::ranges::find(sockets, socket, &OpenSocket::id);
        return it != sockets.end() ? std::optional<int>(it->handle) : std::nullopt;
    }

    /**
     * @brief Forgets every open socket, and tells each that it's closed: the modem restarted, or
     * was told to close them all.
     */
    void dropAllSockets() {
        std::scoped_lock socketLock(socketMutex);
        for (const auto& open : sockets) {
            open.handler(SocketEvent { .type = SocketEventType::Closed });
        }
        sockets.clear();
    }

    Nrf9151Driver(const CellularModemPins& pins, std::unique_ptr<AtChannel> channel, std::vector<Rat> rats)
        : pins(pins)
        , channel(std::move(channel))
        , rats(std::move(rats))
        , powerEvents(xQueueCreate(4, sizeof(PowerEvent)))
        , riGpio(pins.ri->getGpio()) {
        if (powerEvents == nullptr) {
            // Only fails when out of memory, at boot
            esp_system_abort("could not create the modem power queue");
        }
        this->channel->onUrc([this](std::string_view line) {
            handleLine(line);
        });
        this->channel->onActivity([this]() {
            std::scoped_lock lock(powerMutex);
            lastActivity = steady_clock::now();
        });
    }

    Result<> setUpPins() {
        // DTR: deasserted (high) until the first command; the board pulls it up too
        gpio_config_t dtr {
            .pin_bit_mask = 1ULL << pins.dtr->getGpio(),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_TRY(gpio_set_level(pins.dtr->getGpio(), 1));
        ESP_TRY(gpio_config(&dtr));
        // Light sleep only happens with DTR deasserted, which the board's pull-up holds just as well
        ESP_TRY(gpio_sleep_set_direction(pins.dtr->getGpio(), GPIO_MODE_DISABLE));
        ESP_TRY(gpio_sleep_set_pull_mode(pins.dtr->getGpio(), GPIO_FLOATING));

        // nRESET: open-drain, released, and never a pull-up (the nRF9151 has its own, to 2.2 V)
        gpio_config_t reset {
            .pin_bit_mask = 1ULL << pins.reset->getGpio(),
            .mode = GPIO_MODE_OUTPUT_OD,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_TRY(gpio_set_level(pins.reset->getGpio(), 1));
        ESP_TRY(gpio_config(&reset));
        // Released in light sleep too: Hi-Z, no pull
        ESP_TRY(gpio_sleep_set_direction(pins.reset->getGpio(), GPIO_MODE_DISABLE));
        ESP_TRY(gpio_sleep_set_pull_mode(pins.reset->getGpio(), GPIO_FLOATING));

        // RI: active low; the pull-up holds it inactive while the modem resets or boots, when it
        // floats. Only armed once the modem is up (see armRingIndicator())
        gpio_config_t ri {
            .pin_bit_mask = 1ULL << pins.ri->getGpio(),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_TRY(gpio_config(&ri));
        // Keep the pull-up in light sleep too, where RI has to wake us
        ESP_TRY(gpio_sleep_sel_dis(pins.ri->getGpio()));
        ESP_TRY(gpio_isr_handler_add(pins.ri->getGpio(), &Nrf9151Driver::handleRingInterrupt, this));

        Task::run("cellular-power", 3072, [this](Task& /*task*/) {
            managePower();
        });
        return {};
    }

    // ---- UART power (DTR) and wake-up (RI) ----

    enum class PowerEvent : uint8_t {
        // The modem asserted RI
        Ring,
        // DTR was asserted for a command: start watching for idle
        Awake,
    };

    /**
     * @brief Keeps DTR asserted, and the ESP32 awake, while the modem is in use.
     */
    class UartUse {
    public:
        explicit UartUse(Nrf9151Driver& driver)
            : driver(driver) {
            driver.holdUart();
        }

        ~UartUse() {
            driver.releaseUart();
        }

        UartUse(const UartUse&) = delete;
        UartUse& operator=(const UartUse&) = delete;

    private:
        Nrf9151Driver& driver;
    };

    void holdUart() {
        steady_clock::duration offFor {};
        {
            std::scoped_lock lock(powerMutex);
            // First, so that the power task can't turn the UART off from here on
            uartUsers++;
            lastActivity = steady_clock::now();
            if (!dtrAsserted) {
                offFor = steady_clock::now() - dtrDeassertedAt;
            }
        }
        // The modem may still be turning its UART off from the last time DTR went high, and could
        // miss what we send as it does
        if (offFor > steady_clock::duration::zero() && offFor < DTR_MIN_OFF_TIME) {
            Task::delay(duration_cast<milliseconds>(DTR_MIN_OFF_TIME - offFor) + 1ms);
        }
        bool woke;
        {
            std::scoped_lock lock(powerMutex);
            woke = assertDtrLocked();
        }
        if (woke) {
            notifyPower(PowerEvent::Awake);
        }
    }

    void releaseUart() {
        std::scoped_lock lock(powerMutex);
        uartUsers--;
        lastActivity = steady_clock::now();
    }

    /**
     * @return whether DTR was deasserted until now
     */
    bool assertDtrLocked() {
        if (dtrAsserted) {
            return false;
        }
        // Received bytes would be lost in light sleep: the modem may send whenever DTR is low
        awake.emplace(noLightSleep);
        gpio_set_level(pins.dtr->getGpio(), 0);
        dtrAsserted = true;
        return true;
    }

    void notifyPower(PowerEvent event) {
        // Dropped when the queue is full: the task is behind on events that say the same
        xQueueSend(powerEvents, &event, 0);
    }

    static void IRAM_ATTR handleRingInterrupt(void* arg) {
        auto* self = static_cast<Nrf9151Driver*>(arg);
        // Level-triggered: stays off until RI is released again, see managePower()
        gpio_intr_disable(self->riGpio);
        // In case the queue is full: the power task looks at this on every round
        self->ringSignalled = true;
        auto event = PowerEvent::Ring;
        BaseType_t woken = pdFALSE;
        xQueueSendFromISR(self->powerEvents, &event, &woken);
        portYIELD_FROM_ISR(woken);
    }

    /**
     * @brief Arms RI as an interrupt and a light-sleep wake-up source; only once the modem is up,
     * as RI floats while it boots.
     */
    void armRingIndicator() {
        if (riArmed.exchange(true)) {
            return;
        }
        auto gpio = pins.ri->getGpio();
        // Low level: light sleep only wakes on levels, not edges
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_wakeup_enable(gpio, GPIO_INTR_LOW_LEVEL));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_enable_gpio_wakeup());
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_intr_enable(gpio));
    }

    /**
     * @brief Answers RI, and turns the modem's UART off once it's been idle for a while.
     */
    [[noreturn]] void managePower() {
        bool ringPending = false;
        while (true) {
            bool on;
            {
                std::scoped_lock lock(powerMutex);
                on = dtrAsserted;
            }
            PowerEvent event {};
            // While DTR is asserted, look in on the UART regularly, and on RI while waiting for
            // the modem to release it; otherwise only RI matters
            auto wait = portMAX_DELAY;
            if (on || (ringPending && !ringStuck)) {
                wait = pdMS_TO_TICKS(POWER_CHECK_INTERVAL.count());
            } else if (ringPending) {
                wait = pdMS_TO_TICKS(RING_STUCK_POLL_INTERVAL.count());
            }
            bool received = xQueueReceive(powerEvents, &event, wait) == pdTRUE;
            bool rang = received && event == PowerEvent::Ring;
            if (ringSignalled.exchange(false) || rang) {
                std::scoped_lock lock(powerMutex);
                assertDtrLocked();
                lastActivity = steady_clock::now();
                ringPending = true;
            }
            if (ringPending) {
                ringPending = !rearmRingIndicator();
            }
            // A stuck RI doesn't keep the UART on: nothing is coming
            sleepUartIfIdle(ringPending && !ringStuck);
        }
    }

    /**
     * @brief Turns the interrupt back on once the modem has released RI, which it does once its
     * UART is up. RI that stays low (a modem stuck in reset) would keep waking us from light sleep,
     * so after RING_STUCK_TIMEOUT it's taken out of the wake-up sources, and only looked at every
     * RING_STUCK_POLL_INTERVAL, until it goes high.
     *
     * @return whether RI is armed again
     */
    bool rearmRingIndicator() {
        auto gpio = pins.ri->getGpio();
        auto now = steady_clock::now();
        if (gpio_get_level(gpio) == 0) {
            if (!ringLowSince) {
                ringLowSince = now;
            } else if (now - *ringLowSince >= RING_STUCK_TIMEOUT && !ringStuck) {
                LOGTW(CELLULAR, "The modem's ring indicator is stuck low; not waking on it until it goes high");
                gpio_wakeup_disable(gpio);
                ringStuck = true;
            }
            return false;
        }
        if (ringStuck) {
            LOGTI(CELLULAR, "The modem's ring indicator is back to normal");
            gpio_wakeup_enable(gpio, GPIO_INTR_LOW_LEVEL);
            ringStuck = false;
        }
        ringLowSince.reset();
        gpio_intr_enable(gpio);
        return true;
    }

    void sleepUartIfIdle(bool ringPending) {
        std::scoped_lock lock(powerMutex);
        if (!dtrAsserted || !sleepAllowed || uartUsers > 0 || ringPending) {
            return;
        }
        if (steady_clock::now() - lastActivity < UART_IDLE_TIMEOUT) {
            return;
        }
        gpio_set_level(pins.dtr->getGpio(), 1);
        dtrAsserted = false;
        dtrDeassertedAt = steady_clock::now();
        awake.reset();
    }

    // ---- Lines from the modem ----

    static bool isUrc(std::string_view line, std::string_view command) {
        if (line == "Ready") {
            return true;
        }
        static constexpr std::array<std::string_view, 11> URC_PREFIXES {
            "+CEREG:", "+CSCON:", "+CEDRXP:", "+CGEV:", "%MDMEV:", "%XTIME:", "%XMODEMSLEEP:", "%XT3412:",
            "%CESQ:", "#XAPOLL:", "#XSENDNTF:"
        };
        for (auto prefix : URC_PREFIXES) {
            if (line.starts_with(prefix)) {
                // Unless the command asked for it: AT+CEREG? answers with a +CEREG: line too
                auto name = prefix.substr(0, prefix.size() - 1);
                return !(command.starts_with("AT") && command.substr(2).starts_with(name));
            }
        }
        return false;
    }

    // Runs on the UART's reader task
    void handleLine(std::string_view line) {
        if (line == "Ready") {
            // Serial Modem booted: after a reset, a brownout or a crash, everything it had is gone
            restartSeen = true;
            if (started) {
                LOGTW(CELLULAR, "%s restarted", getName());
                restarted = true;
                dropAllSockets();
            }
            return;
        }
        if (auto poll = parseXapoll(line)) {
            // Announcements for sockets that aren't ours (NTP's UDP socket) or not anymore are dropped
            std::scoped_lock socketLock(socketMutex);
            auto it = std::ranges::find(sockets, poll->handle, &OpenSocket::handle);
            if (it != sockets.end()) {
                if (poll->readable()) {
                    it->handler(SocketEvent { .type = SocketEventType::DataAvailable });
                }
                if (poll->closed()) {
                    it->handler(SocketEvent { .type = SocketEventType::Closed });
                }
            }
            return;
        }
        std::scoped_lock lock(handlerMutex);
        if (urcHandler) {
            urcHandler(line);
        }
    }

    // ---- Bring-up and recovery ----

    /**
     * @brief Finds the modem's UART rate, trying both in turn, and leaves the UART on it. Keeps
     * going while the modem boots, which it does together with the board.
     */
    std::optional<int> findBaudRate() {
        auto backoff = WAKE_INITIAL_BACKOFF;
        for (int attempt = 1; attempt <= WAKE_ATTEMPTS; attempt++) {
            if (auto rate = probeBaudRate()) {
                if (attempt > 1) {
                    LOGTV(CELLULAR, "%s answered after %d attempts", getName(), attempt);
                }
                return rate;
            }
            Task::delay(backoff);
            backoff = std::min(backoff * 2, WAKE_MAX_BACKOFF);
        }
        LOGTW(CELLULAR, "%s did not answer after %d attempts at either %d or %d baud", getName(), WAKE_ATTEMPTS, FAST_BAUD_RATE, DEFAULT_BAUD_RATE);
        return std::nullopt;
    }

    /**
     * @brief One AT at each rate, fast first: the modem stays there until it restarts.
     *
     * @return the rate it answered at, which the UART is left on
     */
    std::optional<int> probeBaudRate() {
        for (int rate : { FAST_BAUD_RATE, DEFAULT_BAUD_RATE }) {
            channel->setBaudRate(rate);
            // An AT sent at the other rate reaches the modem as a few garbage bytes without a
            // CR: a bare CR ends that line, whatever it answers
            channel->command("", PROBE_FLUSH_TIMEOUT);
            // Neither the answer to that nor what a probe at the other rate garbled is owed to the
            // next command
            channel->discardInput();
            bool answered = channel->command("AT", WAKE_TIMEOUT).ok();
            channel->discardInput();
            if (answered) {
                return rate;
            }
        }
        return std::nullopt;
    }

    void switchBaudRate() {
        auto set = "AT+IPR=" + std::to_string(FAST_BAUD_RATE);
        LOGTD(CELLULAR, "Switching %s from %d to %d baud", getName(), DEFAULT_BAUD_RATE, FAST_BAUD_RATE);
        // The OK still comes at the old rate; the modem switches straight after
        auto response = channel->command(set, DEFAULT_TIMEOUT);
        if (!response.ok()) {
            LOGTW(CELLULAR, "%s failed: %s %s; staying at %d baud", set.c_str(), toString(response.result), response.error.c_str(), DEFAULT_BAUD_RATE);
            channel->discardInput();
            return;
        }
        channel->setBaudRate(FAST_BAUD_RATE);
        channel->discardInput();
    }

    /**
     * @brief Pulses nRESET, and waits for Serial Modem to boot. Call with commandMutex held and
     * the UART in use, so that DTR is asserted and the booting modem turns its UART on.
     */
    void reset() {
        restartSeen = false;
        gpio_set_level(pins.reset->getGpio(), 0);
        Task::delay(RESET_PULSE);
        gpio_set_level(pins.reset->getGpio(), 1);
        lastReset = steady_clock::now();
        channel->setBaudRate(DEFAULT_BAUD_RATE);
        channel->discardInput();
        auto deadline = steady_clock::now() + BOOT_TIMEOUT;
        while (!restartSeen && steady_clock::now() < deadline) {
            Task::delay(50ms);
        }
        if (!restartSeen) {
            LOGTW(CELLULAR, "%s did not say Ready after the reset", getName());
        }
        dropAllSockets();
    }

    /**
     * @brief Whether a reset is allowed now: not more often than MIN_RESET_INTERVAL, so that a
     * modem that is gone for good doesn't get reset over and over.
     */
    bool mayReset() const {
        return !lastReset || steady_clock::now() - *lastReset >= MIN_RESET_INTERVAL;
    }

    /**
     * @brief Finds out why commands go unanswered, once several have in a row.
     *
     * The modem may have restarted: Serial Modem boots at DEFAULT_BAUD_RATE, so its "Ready"
     * arrives garbled at the fast rate, and its answers don't come at all. A probe at both rates
     * tells that apart from a modem that's busy, or gone. A modem that restarted is set up again
     * by the next command; one that doesn't answer at all is reset, at most every
     * MIN_RESET_INTERVAL.
     */
    void resetIfUnresponsive() {
        if (++unansweredCommands < UNANSWERED_BEFORE_RESET) {
            return;
        }
        unansweredCommands = 0;
        // The command that just timed out is owed an answer that won't come as long as the rates
        // don't match; the probe mustn't wait for it
        channel->discardInput();
        auto rate = probeBaudRate();
        if (rate == FAST_BAUD_RATE) {
            LOGTD(CELLULAR, "%s answers again", getName());
            return;
        }
        dropAllSockets();
        if (rate == DEFAULT_BAUD_RATE) {
            LOGTW(CELLULAR, "%s restarted (it's back at %d baud)", getName(), DEFAULT_BAUD_RATE);
        } else if (mayReset()) {
            LOGTW(CELLULAR, "%s stopped answering, resetting it", getName());
            reset();
        } else {
            LOGTW(CELLULAR, "%s stopped answering; the last reset was less than %lld minutes ago", getName(),
                static_cast<long long>(duration_cast<minutes>(MIN_RESET_INTERVAL).count()));
            // Back to the rate it was last on, in case it's only busy
            channel->setBaudRate(FAST_BAUD_RATE);
            return;
        }
        restarted = started.load();
    }

    /**
     * @brief After the modem restarted on its own, or was reset, sets it up again the way the
     * last start(), configure() and configurePowerSaving() left it. Call with commandMutex held.
     */
    void recoverIfRestarted() {
        if (!restarted.exchange(false)) {
            return;
        }
        LOGTI(CELLULAR, "Setting %s up again", getName());
        if (!startLocked()) {
            // Until more commands go unanswered, see resetIfUnresponsive()
            LOGTW(CELLULAR, "%s is still not answering", getName());
            return;
        }
        // Only what had been set up before the restart: a restart during the first configure()
        // is set up by the rest of it, and the next command
        if (configured) {
            configureLocked(allowSleep);
        }
        if (powerSavingConfigured) {
            configurePowerSavingLocked(edrxCycle);
        }
    }

    bool ensureSystemMode() {
        auto mode = toSystemMode(rats);
        if (!mode) {
            return false;
        }
        auto response = at("AT%XSYSTEMMODE?", DEFAULT_TIMEOUT);
        auto current = response.ok() ? response.find("%XSYSTEMMODE:") : std::nullopt;
        if (current == *mode) {
            return true;
        }
        LOGTI(CELLULAR, "Setting the system mode to %s (was: %s)", mode->c_str(),
            current ? std::string(*current).c_str() : "?");
        // Only changes with the radio off; CFUN=4 rather than 0, which would write to flash
        if (!radioOff()) {
            return false;
        }
        return apply("AT%XSYSTEMMODE=" + *mode);
    }

    bool ensureReleaseAssistance() {
        // Lets AT#XSOCKETOPT tell the network when we're done (releaseRrc())
        auto response = at("AT%RAI?", DEFAULT_TIMEOUT);
        if (response.ok() && response.find("%RAI:") == "1") {
            return true;
        }
        return apply("AT%RAI=1");
    }

    bool radioOff() {
        auto response = at("AT+CFUN?", DEFAULT_TIMEOUT);
        if (response.ok() && response.find("+CFUN:") != "1") {
            return true;
        }
        return apply("AT+CFUN=4", CFUN_TIMEOUT);
    }

    bool ensureFullFunctionality() {
        // Serial Modem starts with the radio off (CFUN=0)
        auto response = at("AT+CFUN?", DEFAULT_TIMEOUT);
        if (response.ok() && response.find("+CFUN:") == "1") {
            return true;
        }
        return apply("AT+CFUN=1", CFUN_TIMEOUT);
    }

    // ---- Helpers ----

    bool apply(const std::string& setting, milliseconds timeout = DEFAULT_TIMEOUT) {
        auto response = at(setting, timeout);
        if (!response.ok()) {
            LOGTW(CELLULAR, "%s failed: %s %s", setting.c_str(), toString(response.result), response.error.c_str());
        }
        return response.ok();
    }

    /**
     * @brief Decodes the hex data line that follows the header of a hex mode read.
     *
     * @return the byte count, or nullopt if there's no header, or the data doesn't match it
     */
    static std::optional<size_t> readHexData(const AtResponse& response, std::string_view prefix, int handle, uint8_t* buffer, size_t capacity) {
        for (size_t i = 0; i < response.lines.size(); i++) {
            auto size = parseXrecvHeader(response.lines[i], prefix, handle);
            if (!size) {
                continue;
            }
            if (*size == 0) {
                return 0;
            }
            if (i + 1 >= response.lines.size() || *size > capacity) {
                return std::nullopt;
            }
            const auto& hex = response.lines[i + 1];
            if (hex.size() != *size * 2 || !fromHex(hex, buffer)) {
                return std::nullopt;
            }
            return size;
        }
        return std::nullopt;
    }

    /**
     * @brief The first response line the decoder accepts, decoded.
     */
    template <typename Decoder>
    static auto findDecoded(const AtResponse& response, Decoder decode) -> decltype(decode(std::string_view {})) {
        for (const auto& line : response.lines) {
            if (auto decoded = decode(line)) {
                return decoded;
            }
        }
        return std::nullopt;
    }

    static std::optional<std::vector<AtField>> findFields(const AtResponse& response, std::string_view prefix) {
        for (const auto& line : response.lines) {
            if (auto fields = parseAtFields(line, prefix)) {
                return fields;
            }
        }
        return std::nullopt;
    }

    void logSimResponse(const std::string& command) {
        for (int attempt = 1; attempt < SIM_ATTEMPTS; attempt++) {
            if (at(command, DEFAULT_TIMEOUT).result != AtResult::CmeError) {
                break;
            }
            Task::delay(SIM_RETRY_DELAY);
        }
        logResponse(command);
    }

    void logResponse(const std::string& command, milliseconds timeout = DEFAULT_TIMEOUT) {
        auto response = at(command, timeout);
        if (!response.ok()) {
            LOGTW(CELLULAR, "%s: %s %s", command.c_str(), toString(response.result), response.error.c_str());
            return;
        }
        for (const auto& line : response.lines) {
            LOGTI(CELLULAR, "%s: %s", command.c_str(), line.c_str());
        }
    }

    // Serial Modem's rate after every boot, and the fastest the ESP32's UART divides cleanly from
    // its 40 MHz crystal of the rates AT+IPR takes (43.4, 0.06% off with the fractional divider)
    static constexpr int DEFAULT_BAUD_RATE = 115200;
    static constexpr int FAST_BAUD_RATE = 921600;

    // Most commands answer well within this
    static constexpr milliseconds DEFAULT_TIMEOUT = 5s;
    static constexpr milliseconds CFUN_TIMEOUT = 30s;
    // Resolving the hostname and the TCP handshake, in poor coverage
    static constexpr milliseconds CONNECT_TIMEOUT = 75s;
    // Until the data is in the modem's buffer
    static constexpr milliseconds SEND_TIMEOUT = 30s;
    // Non-blocking; a 4 KB hex line takes 45 ms at 921600 baud
    static constexpr milliseconds RECEIVE_TIMEOUT = 5s;
    static constexpr milliseconds NTP_WAIT = 5s;
    static constexpr milliseconds SLOW_COMMAND = 1s;
    static constexpr int SIM_ATTEMPTS = 10;
    static constexpr milliseconds SIM_RETRY_DELAY = 500ms;

    static constexpr int WAKE_ATTEMPTS = 10;
    static constexpr milliseconds WAKE_TIMEOUT = 300ms;
    static constexpr milliseconds WAKE_INITIAL_BACKOFF = 100ms;
    static constexpr milliseconds WAKE_MAX_BACKOFF = 1s;
    static constexpr milliseconds PROBE_FLUSH_TIMEOUT = 100ms;

    // nRESET pulse; Serial Modem takes about 2 s to boot after it
    static constexpr milliseconds RESET_PULSE = 10ms;
    static constexpr milliseconds BOOT_TIMEOUT = 10s;
    static constexpr int UNANSWERED_BEFORE_RESET = 3;
    static constexpr milliseconds MIN_RESET_INTERVAL = 10min;

    // How long the UART stays on after the last byte either way. The modem's answers to what we
    // just sent tend to come within this; anything later arrives through RI
    static constexpr milliseconds UART_IDLE_TIMEOUT = 100ms;
    static constexpr milliseconds POWER_CHECK_INTERVAL = 20ms;
    static constexpr milliseconds RING_STUCK_TIMEOUT = 2s;
    // How long DTR stays high before it's asserted again, for the modem to finish turning its
    // UART off
    static constexpr milliseconds DTR_MIN_OFF_TIME = 10ms;
    static constexpr milliseconds RING_STUCK_POLL_INTERVAL = 10s;

    const CellularModemPins pins;
    const std::unique_ptr<AtChannel> channel;
    const std::vector<Rat> rats;

    // Serializes commands, and keeps sequences of them together
    std::mutex commandMutex;
    // Guarded by commandMutex
    bool allowSleep = true;
    std::optional<milliseconds> edrxCycle;
    bool powerSavingConfigured = false;
    int unansweredCommands = 0;
    std::optional<steady_clock::time_point> lastReset;

    // Set from the reader task
    std::atomic<bool> configured { false };
    // Whether start() found the modem: a restart after that needs setting up again
    std::atomic<bool> started { false };
    std::atomic<bool> restarted { false };
    std::atomic<bool> restartSeen { false };

    struct OpenSocket {
        SocketId id;
        // The modem's handle
        int handle;
        SocketEventHandler handler;
    };

    // Guards the open sockets, which the reader task looks up announcements in. Taken after
    // commandMutex, never before
    std::mutex socketMutex;
    std::vector<OpenSocket> sockets;
    SocketId nextSocketId = 1;

    // Guards the URC handler
    std::mutex handlerMutex;
    UrcHandler urcHandler;

    // Guards the UART power state below
    std::mutex powerMutex;
    PowerManagementLock noLightSleep { "cellular", ESP_PM_NO_LIGHT_SLEEP };
    std::optional<PowerManagementLockGuard> awake;
    bool dtrAsserted = false;
    steady_clock::time_point dtrDeassertedAt;
    // Until configure(): the UART stays on while the modem is being set up
    bool sleepAllowed = false;
    int uartUsers = 0;
    steady_clock::time_point lastActivity;

    QueueHandle_t powerEvents;
    std::atomic<bool> riArmed { false };
    // Set by the RI interrupt, which can't block on a full queue
    std::atomic<bool> ringSignalled { false };
    // For the interrupt, which shouldn't go through a shared_ptr
    const gpio_num_t riGpio;
    // Only touched on the power task
    std::optional<steady_clock::time_point> ringLowSince;
    bool ringStuck = false;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
