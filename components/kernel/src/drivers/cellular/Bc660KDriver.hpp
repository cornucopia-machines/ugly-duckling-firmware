#pragma once

#include <Log.hpp>
#include <PowerManager.hpp>
#include <Task.hpp>
#include <drivers/cellular/AtResponse.hpp>
#include <drivers/cellular/AtSocket.hpp>
#include <drivers/cellular/CellularModuleDriver.hpp>
#include <drivers/cellular/Cereg.hpp>
#include <drivers/cellular/Edrx.hpp>
#include <drivers/cellular/EspModem.hpp>
#include <drivers/cellular/NetworkTime.hpp>
#include <drivers/cellular/RadioStatus.hpp>

#include <driver/uart.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <initializer_list>
#include <iterator>
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

inline const char* describe(esp_modem::terminal_error error) {
    switch (error) {
        case esp_modem::terminal_error::BUFFER_OVERFLOW:
            return "receive overflow";
        case esp_modem::terminal_error::CHECKSUM_ERROR:
            return "parity error";
        case esp_modem::terminal_error::UNEXPECTED_CONTROL_FLOW:
            return "framing error or break";
        case esp_modem::terminal_error::DEVICE_GONE:
            return "device gone";
    }
    return "unknown";
}

/**
 * @brief Quectel BC660K-GL NB-IoT module, on the Desert Lark daughter board.
 *
 * Derives from esp_modem's GenericModule for its DTE plumbing. The commands here go through
 * command(), which parses the whole response itself rather than looking for "OK" anywhere in it.
 *
 * Socket data goes over the UART as hex (AT+QICFG="dataformat",1,1): that keeps every send and
 * every read a single text line, so it travels through the same command path as everything else,
 * and binary data never reaches the line parser. It costs twice the bytes on the UART, which runs
 * far faster than NB-IoT anyway, and caps a send at 1024 bytes instead of 2048; the bytes over
 * the air are the same.
 *
 * Bench notes on what the driver has to handle are in
 * https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/641
 */
class Bc660KDriver final : public CellularModuleDriver, public esp_modem::GenericModule {
public:
    /**
     * @param port the UART the DTE runs on, set up at DEFAULT_BAUD_RATE; start() changes its rate
     * @param pushSocketData whether to open sockets in direct push mode, see pushesSocketData()
     */
    Bc660KDriver(const std::shared_ptr<esp_modem::DTE>& dte, uart_port_t port, bool pushSocketData)
        : GenericModule(dte, std::make_unique<esp_modem::PdpContext>(""))
        , port(port)
        , pushSocketData(pushSocketData) {
        dte->set_urc_cb([this](uint8_t* data, size_t len) {
            return processUrcData(std::string_view(reinterpret_cast<const char*>(data), len));
        });
        // On an overflow, esp_modem flushes everything the UART has buffered, which can take the
        // rest of an AT+QIRD response with it. It logs these under uart_terminal, which is turned
        // down to ERROR to keep out its warnings about light-sleep wakeups
        dte->set_error_cb([this](esp_modem::terminal_error error) {
            uartErrors++;
            LOGTW(CELLULAR, "UART error from the modem: %s", describe(error));
        });
    }

    const char* getName() const override {
        return "BC660K-GL";
    }

    /**
     * The module has no autobaud, and AT+IPR is saved to its NVRAM, so it can be on either rate:
     * the default on a new module, the fast one ever after. Nothing documented resets it, not
     * even RESET_N. Each switch is confirmed at the new rate, and undone if that fails, so a link
     * that can't take the fast rate costs speed, never the module.
     */
    bool start() override {
        PowerManagementLockGuard awake(noLightSleep);
        std::scoped_lock lock(commandMutex);
        auto rate = findBaudRateLocked();
        if (!rate) {
            return false;
        }
        if (*rate == FAST_BAUD_RATE) {
            LOGTD(CELLULAR, "%s is answering at %d baud", getName(), FAST_BAUD_RATE);
            return true;
        }
        switchBaudRateLocked();
        // Whichever way the switch went, find the module again; at the fast rate, the first AT
        // answers
        return findBaudRateLocked().has_value();
    }

    bool wake() override {
        PowerManagementLockGuard awake(noLightSleep);
        std::scoped_lock lock(commandMutex);
        return wakeLocked();
    }

    bool configure(bool allowSleep) override {
        // First, since they may restart the module, which loses the settings below
        bool success = ensureRxdWakeup();
        success = ensureReleaseVersion() && success;

        // None of these is ever wrong to repeat, so there's no point checking first
        static constexpr std::array SETTINGS {
            // Echo is on after power-on; off keeps the command out of every response
            "ATE0",
            // A readable "+CME ERROR: <text>" instead of a bare ERROR
            "AT+CMEE=2",
            // Only URC mode 3 carries the EMM reject cause, which is what tells "no coverage"
            // apart from "subscription refused"
            "AT+CEREG=3",
            // Report entering and leaving PSM
            "AT+QNBIOTEVENT=1,1",
            // Report the RRC connection going up and down (+CSCON), for the debug console and
            // for telemetry on how long the radio stays connected
            "AT+CSCON=1",
        };

        // Let the module light sleep between paging occasions; it wakes on the network's schedule
        // and on UART activity from us (the first command after that is lost, see wakeLocked()).
        // Not deep sleep: that only happens in PSM, which is off. Not saved to NVRAM, so it has to
        // be sent after every module restart. The AT manual recommends turning sleep off for data
        // communication (AT+QSCLK, note 3): with it on, received data was announced late during
        // downloads, and the receive buffer overflowed (docs/specs/NB-IoT.md)
        const char* sleep = allowSleep ? "AT+QSCLK=2" : "AT+QSCLK=0";
        if (!allowSleep) {
            LOGTI(CELLULAR, "Keeping the modem out of sleep for this boot");
        }

        auto apply = [&](const char* setting) {
            auto response = command(setting, DEFAULT_TIMEOUT);
            if (!response.ok()) {
                LOGTW(CELLULAR, "%s failed: %s %s", setting, toString(response.result), response.error.c_str());
                success = false;
            }
        };
        for (const char* setting : SETTINGS) {
            apply(setting);
        }
        apply(sleep);

        // These two are saved to NVRAM and slow to apply, so only write them when they differ
        success = ensureFullFunctionality() && success;
        success = ensureBands() && success;

        // Also saved to NVRAM, so only written when they differ
        for (const auto& setting : PERSISTED_SETTINGS) {
            success = ensureSetting(setting) && success;
        }
        return success;
    }

    bool configurePowerSaving(std::optional<milliseconds> edrxCycle) override {
        // In PSM the module is unreachable until its own next wake, and it doesn't report URCs;
        // eDRX keeps it reachable with a bounded delay instead
        bool success = ensureSetting({ .query = "AT+CPSMS?", .expected = "+CPSMS: 0", .set = "AT+CPSMS=0" });
        return ensureEdrx(edrxCycle) && success;
    }

    AtResponse command(const std::string& command, milliseconds timeout) override {
        bool inPause = awaitPushPause(command);
        auto response = this->command(command, timeout, std::string_view {});
        if (inPause) {
            std::scoped_lock lock(pushGateMutex);
            commandsInPause--;
            pushGate.notify_all();
        }
        return response;
    }

    void onUrc(UrcHandler handler) override {
        std::scoped_lock lock(urcHandlerMutex);
        urcHandler = std::move(handler);
    }

    void logStatus() override {
        // Manufacturer, model and firmware revision
        logResponse("ATI");
        // IMEI: the device identity the network (and 1NCE's IMEI lock) sees
        logResponse("AT+CGSN=1");
        // SIM: inserted and unlocked, and which one it is
        logResponse("AT+CPIN?");
        logResponse("AT+CIMI");
        logResponse("AT+QCCID");
        // 3GPP release and UE category: Cat NB2 (release 14) allows about five times the downlink
        // of Cat NB1, if the network supports it
        logResponse(R"(AT+QCFG="relversion")");
        logResponse(R"(AT+QCFG="NBcategory")");
    }

    std::optional<ServingCell> logRadioStatus() override {
        auto quality = findDecoded(command("AT+CSQ", DEFAULT_TIMEOUT), parseCsq);
        auto cell = queryServingCell();

        std::string rssi = quality && quality->rssiDbm ? std::to_string(*quality->rssiDbm) + " dBm" : "unknown";
        std::string serving = cell ? describe(*cell) : "no AT+QENG=0 answer";
        LOGTI(CELLULAR, "Radio: RSSI %s, %s", rssi.c_str(), serving.c_str());

        // EMM state, PLMN search state, and which network it is trying: tells "nothing found"
        // apart from "found an operator that turns us away"
        logResponse("AT+QENG=3", QENG_TIMEOUT);
        return cell;
    }

    std::optional<ServingCell> queryServingCell() override {
        // Numeric mode only: the "servingcell" form belongs to Quectel's LTE modules and returns
        // ERROR here
        return findDecoded(command("AT+QENG=0", QENG_TIMEOUT), parseQengServingCell);
    }

    std::optional<EdrxParameters> queryEdrx() override {
        return findDecoded(command("AT+CEDRXRDP", DEFAULT_TIMEOUT), parseCedrxrdp);
    }

    std::optional<milliseconds> queryIdlePagingCycle() override {
        return findDecoded(command("AT+QDRX?", DEFAULT_TIMEOUT), parseQdrxIdleCycle);
    }

    std::optional<bool> queryRrcConnected() override {
        return findDecoded(command("AT+CSCON?", DEFAULT_TIMEOUT), parseCsconRead);
    }

    std::optional<std::string> queryIpAddress() override {
        // Context 0 is the default bearer the module sets up when it attaches
        auto response = command("AT+CGPADDR=0", DEFAULT_TIMEOUT);
        auto fields = response.ok() ? findFields(response, "+CGPADDR:") : std::nullopt;
        if (!fields || fields->size() < 2) {
            return std::nullopt;
        }
        auto address = (*fields)[1].asString();
        return address ? std::optional<std::string>(*address) : std::nullopt;
    }

    bool prepareNetwork() override {
        // There is no default DNS server: hostnames only resolve if the network handed one out
        // with the PDP context, or we configure one. The setting isn't saved, so check every time
        auto response = command("AT+QIDNSCFG=0", DEFAULT_TIMEOUT);
        auto fields = response.ok() ? findFields(response, "+QIDNSCFG:") : std::nullopt;
        auto primary = fields && fields->size() > 1 ? (*fields)[1].asString() : std::nullopt;
        if (primary && *primary != "0.0.0.0") {
            LOGTD(CELLULAR, "DNS: %s", response.lines.front().c_str());
            return true;
        }
        LOGTI(CELLULAR, "The network gave us no DNS server (%s), using public ones",
            response.lines.empty() ? "?" : response.lines.front().c_str());
        response = command(R"(AT+QIDNSCFG=0,"8.8.8.8","1.1.1.1")", DEFAULT_TIMEOUT);
        if (!response.ok()) {
            LOGTW(CELLULAR, "Could not configure DNS: %s %s", toString(response.result), response.error.c_str());
            return false;
        }
        return true;
    }

    std::optional<time_t> queryNtpTime(const std::string& server) override {
        auto query = "AT+QNTP=0,\"" + server + "\"";
        auto response = command(query, NTP_TIMEOUT, "+QNTP:");
        if (!response.ok() || response.lines.empty()) {
            LOGTW(CELLULAR, "%s: %s %s", query.c_str(), toString(response.result), response.error.c_str());
            return std::nullopt;
        }
        // Logged raw until the bench confirms whether the module reports UTC or local time here
        LOGTD(CELLULAR, "%s: %s", query.c_str(), response.lines.back().c_str());
        auto time = parseQntp(response.lines.back());
        if (!time) {
            LOGTW(CELLULAR, "%s failed: %s", query.c_str(), response.lines.back().c_str());
        }
        return time;
    }

    bool openSocket(const std::string& host, int port) override {
        // A connection left over from before (a reconnect after an error) would keep the ID busy
        closeSocket();
        auto open = "AT+QIOPEN=0," + std::to_string(CONNECT_ID) + R"(,"TCP",")" + host + "\"," + std::to_string(port)
            // Local port assigned automatically; direct push mode (1) or buffer access mode (0)
            + ",0," + (pushSocketData ? "1" : "0");
        if (pushSocketData) {
            // Pushed data arrives whenever the network delivers it, and waking from light sleep
            // on UART edges loses the first bytes
            awakeWhilePushing.emplace(noLightSleep);
            setPushSocketOpen(true);
        }
        auto response = command(open, QIOPEN_TIMEOUT, "+QIOPEN:");
        if (!response.ok() || response.lines.empty()) {
            LOGTW(CELLULAR, "Could not connect to %s:%d: %s %s", host.c_str(), port, toString(response.result), response.error.c_str());
            closeSocket();
            return false;
        }
        auto result = parseQiopen(response.lines.back(), CONNECT_ID);
        if (result != 0) {
            // Error codes are listed in chapter 3 of the TCP/IP application note
            LOGTW(CELLULAR, "Could not connect to %s:%d: %s", host.c_str(), port, response.lines.back().c_str());
            closeSocket();
            return false;
        }
        LOGTD(CELLULAR, "Connected to %s:%d", host.c_str(), port);
        return true;
    }

    void closeSocket() override {
        // Answers ERROR when the connection is already closed, which is fine
        command("AT+QICLOSE=" + std::to_string(CONNECT_ID), QICLOSE_TIMEOUT, "CLOSE OK");
        setPushSocketOpen(false);
        awakeWhilePushing.reset();
    }

    bool pushesSocketData() const override {
        return pushSocketData;
    }

    bool send(const uint8_t* data, size_t length) override {
        letHeldBackCommandsThrough();
        auto sendCommand = "AT+QISEND=" + std::to_string(CONNECT_ID) + "," + std::to_string(length) + ",\"" + toHex(data, length) + "\"";
        for (int attempt = 1; attempt <= SEND_ATTEMPTS; attempt++) {
            auto response = command(sendCommand, QISEND_TIMEOUT, "SEND ");
            if (response.ok() && !response.lines.empty() && response.lines.back() == "SEND OK") {
                return true;
            }
            // SEND FAIL: the command was fine, but the data didn't make it into the TCP stack,
            // presumably because its buffer is full
            LOGTW(CELLULAR, "Sending %zu bytes failed (attempt %d/%d): %s %s", length, attempt, SEND_ATTEMPTS,
                toString(response.result), response.lines.empty() ? response.error.c_str() : response.lines.back().c_str());
            if (!response.ok()) {
                return false;
            }
            Task::delay(SEND_RETRY_DELAY);
        }
        return false;
    }

    size_t getMaxSendSize() const override {
        // In hex mode; twice that in text mode
        return 1024;
    }

    std::optional<SocketReceive> receive(uint8_t* buffer, size_t length) override {
        length = std::min(length, getMaxReceiveSize());
        auto read = "AT+QIRD=" + std::to_string(CONNECT_ID) + "," + std::to_string(length);
        auto response = command(read, QIRD_TIMEOUT);
        if (!response.ok()) {
            LOGTW(CELLULAR, "%s: %s %s", read.c_str(), toString(response.result), response.error.c_str());
            return std::nullopt;
        }
        for (const auto& line : response.lines) {
            auto data = parseQird(line);
            if (!data) {
                continue;
            }
            if (data->length > length || !fromHex(data->hex, buffer)) {
                LOGTW(CELLULAR, "%s: malformed response '%.32s...'", read.c_str(), line.c_str());
                return std::nullopt;
            }
            // Without the remaining length, a full read is the only sign there may be more
            bool more = data->remaining ? *data->remaining > 0 : data->length == length;
            return SocketReceive { .length = data->length, .more = more, .remaining = data->remaining };
        }
        // The data was taken out of the module's buffer either way, so the stream has a gap now
        LOGTW(CELLULAR, "%s: no data line in the response, %zu lines%s%.40s", read.c_str(), response.lines.size(),
            response.lines.empty() ? "" : ", the first: ", response.lines.empty() ? "" : response.lines.front().c_str());
        return std::nullopt;
    }

    uint32_t getUartErrorCount() const override {
        return uartErrors.load();
    }

    size_t getMaxReceiveSize() const override {
        return 512;
    }

    void onSocketEvent(SocketEventHandler handler) override {
        std::scoped_lock lock(urcHandlerMutex);
        socketEventHandler = std::move(handler);
    }

private:
    /**
     * @brief A setting the module keeps in NVRAM, so we read it and only write when it differs.
     */
    struct PersistedSetting {
        const char* query;
        // The first response line to the query when the setting is right, or its leading fields
        std::string_view expected;
        const char* set;
    };

    static constexpr std::array PERSISTED_SETTINGS {
        // Socket data as hex in both directions (see the class comment)
        PersistedSetting { .query = R"(AT+QICFG="dataformat")", .expected = R"(+QICFG: "dataformat",1,1)", .set = R"(AT+QICFG="dataformat",1,1)" },
        // AT+QIRD and the recv URC report how much data is left in the module's buffer
        PersistedSetting { .query = R"(AT+QICFG="showlength")", .expected = R"(+QICFG: "showlength",1)", .set = R"(AT+QICFG="showlength",1)" },
        // AT+QIRD returns its header and data on one line, which the parser relies on
        PersistedSetting { .query = R"(AT+QICFG="viewmode")", .expected = R"(+QICFG: "viewmode",0)", .set = R"(AT+QICFG="viewmode",0)" },
        // Report network time (NITZ) in UTC as +CTZEU. Only takes effect if set before the module
        // camps on a cell, so on the first boot after the change it applies from the next attach
        PersistedSetting { .query = "AT+CTZR?", .expected = "+CTZR: 3", .set = "AT+CTZR=3" },
    };

    void setPushSocketOpen(bool open) {
        std::scoped_lock lock(pushGateMutex);
        pushSocketOpen = open;
        pushGate.notify_all();
    }

    /**
     * @brief Holds back a command from outside the socket code while a socket pushes data, until
     * a pause in it (see letHeldBackCommandsThrough()) or until the socket closes.
     *
     * Pushed data can arrive at any moment, and a command's response would interleave with it:
     * a URC cut off by the command's response can't be put back together. Socket commands go
     * through anyway, at points where the peer waits for us (sending a request, closing).
     * Only used for downloading updates, after which the device restarts.
     *
     * @return whether the command runs in a pause, and has to be counted off when it's done
     */
    bool awaitPushPause(const std::string& command) {
        std::unique_lock lock(pushGateMutex);
        if (!pushSocketOpen) {
            return false;
        }
        if (!inPushPause) {
            LOGTD(CELLULAR, "%s waits for a pause in the data pushed", command.c_str());
            commandsHeldBack++;
            pushGate.wait(lock, [this] {
                return !pushSocketOpen || inPushPause;
            });
            commandsHeldBack--;
            if (!pushSocketOpen) {
                return false;
            }
        }
        commandsInPause++;
        pushGate.notify_all();
        return true;
    }

    /**
     * @brief Lets the commands held back by awaitPushPause() through before sending.
     *
     * When we send, the peer waits for us: everything it had to send for the previous request
     * (a range of the update image) has arrived, and it sends nothing more until this request
     * reaches it. So nothing is pushed until the send, and commands can run without data getting
     * in between.
     */
    void letHeldBackCommandsThrough() {
        std::unique_lock lock(pushGateMutex);
        if (!pushSocketOpen || commandsHeldBack == 0) {
            return;
        }
        inPushPause = true;
        pushGate.notify_all();
        auto deadline = steady_clock::now() + MAX_PUSH_PAUSE;
        auto idle = [this] {
            return commandsHeldBack == 0 && commandsInPause == 0;
        };
        while (pushGate.wait_until(lock, deadline, idle)) {
            // A task tends to send a few commands in a row; give the next one a moment to come
            if (!pushGate.wait_for(lock, PUSH_PAUSE_GRACE, [&] { return !idle(); })) {
                break;
            }
        }
        // Commands let through already finish before the send
        inPushPause = false;
        pushGate.wait(lock, [this] {
            return commandsInPause == 0;
        });
    }

    /**
     * @param awaitAfterOk see parseAtResponse()
     */
    AtResponse command(const std::string& command, milliseconds timeout, std::string_view awaitAfterOk) {
        // The UART driver only keeps the ESP32 awake while transmitting; in light sleep the
        // response would be lost, as waking on UART edges loses the first bytes
        PowerManagementLockGuard awake(noLightSleep);
        auto requestedAt = steady_clock::now();
        std::scoped_lock lock(commandMutex);
        // While a socket is busy, a command holding the module for long keeps AT+QIRD from
        // emptying its 2 KB receive buffer, and once that's full, received data is lost
        auto waited = steady_clock::now() - requestedAt;
        if (waited >= SLOW_COMMAND) {
            LOGTD(CELLULAR, "%s waited %lld ms for %s", command.c_str(),
                static_cast<long long>(duration_cast<milliseconds>(waited).count()), lastCommand.c_str());
        }
        lastCommand = command;
        // The module can be asleep before any command, and the first AT only wakes it. Skipping
        // that when the module had answered just before didn't work out: right after registering,
        // it ignored a command sent less than a second after its last answer
        if (!wakeLocked()) {
            return AtResponse { .result = AtResult::Timeout, .lines = {}, .error = {} };
        }
        auto startedAt = steady_clock::now();
        auto response = send(command, timeout, awaitAfterOk);
        auto took = steady_clock::now() - startedAt;
        if (took >= SLOW_COMMAND) {
            LOGTD(CELLULAR, "%s took %lld ms", command.c_str(),
                static_cast<long long>(duration_cast<milliseconds>(took).count()));
        }
        return response;
    }

    /**
     * @return nullopt if the setting is right, otherwise the first line of what the module
     * answered, for the log
     */
    std::optional<std::string> checkSetting(const PersistedSetting& setting) {
        auto response = command(setting.query, DEFAULT_TIMEOUT);
        if (response.ok() && !response.lines.empty()) {
            std::string_view line = response.lines.front();
            if (line == setting.expected || (line.starts_with(setting.expected) && line[setting.expected.size()] == ',')) {
                return std::nullopt;
            }
        }
        return response.lines.empty() ? "?" : response.lines.front();
    }

    bool ensureSetting(const PersistedSetting& setting) {
        auto current = checkSetting(setting);
        if (!current) {
            return true;
        }
        LOGTI(CELLULAR, "Setting %s (was: %s)", setting.set, current->c_str());
        auto response = command(setting.set, DEFAULT_TIMEOUT);
        if (!response.ok()) {
            LOGTW(CELLULAR, "%s failed: %s %s", setting.set, toString(response.result), response.error.c_str());
            return false;
        }
        return true;
    }

    bool wakeLocked() {
        // A falling edge on MAIN_RXD wakes the module from deep sleep, and the AT that caused it is
        // lost; the hardware design guide says to keep sending AT until OK comes back. The same
        // loop covers a module that is still booting: it powers up together with the board.
        auto backoff = WAKE_INITIAL_BACKOFF;
        for (int attempt = 1; attempt <= WAKE_ATTEMPTS; attempt++) {
            if (send("AT", WAKE_TIMEOUT).ok()) {
                if (attempt > 1) {
                    LOGTD(CELLULAR, "%s answered after %d attempts", getName(), attempt);
                }
                return true;
            }
            Task::delay(backoff);
            backoff = std::min(backoff * 2, WAKE_MAX_BACKOFF);
        }
        LOGTW(CELLULAR, "%s did not answer after %d attempts", getName(), WAKE_ATTEMPTS);
        return false;
    }

    /**
     * @brief Finds the module's UART rate, trying both in turn: like wakeLocked(), it keeps going
     * while the module boots, which it does together with the board.
     *
     * @return the rate it answered at, which the UART is left on
     */
    std::optional<int> findBaudRateLocked() {
        auto backoff = WAKE_INITIAL_BACKOFF;
        for (int attempt = 1; attempt <= WAKE_ATTEMPTS; attempt++) {
            // Fast first: once switched, the module stays there
            for (int rate : { FAST_BAUD_RATE, DEFAULT_BAUD_RATE }) {
                setHostBaudRate(rate);
                // An AT sent at the other rate reaches the module as a few garbage bytes without a
                // CR, which it keeps: the next AT would extend that line, and get ERROR. A bare CR
                // ends it; its answer, ERROR or nothing, doesn't matter
                send("", PROBE_FLUSH_TIMEOUT);
                if (send("AT", WAKE_TIMEOUT).ok()) {
                    if (attempt > 1) {
                        LOGTD(CELLULAR, "%s answered after %d attempts", getName(), attempt);
                    }
                    return rate;
                }
            }
            Task::delay(backoff);
            backoff = std::min(backoff * 2, WAKE_MAX_BACKOFF);
        }
        LOGTW(CELLULAR, "%s did not answer after %d attempts at either %d or %d baud", getName(), WAKE_ATTEMPTS, FAST_BAUD_RATE, DEFAULT_BAUD_RATE);
        return std::nullopt;
    }

    /**
     * @brief Moves the module and the UART from DEFAULT_BAUD_RATE to FAST_BAUD_RATE, and back if
     * the module doesn't answer at the fast rate.
     */
    void switchBaudRateLocked() {
        auto set = "AT+IPR=" + std::to_string(FAST_BAUD_RATE);
        LOGTI(CELLULAR, "%s is answering at %d baud, switching it to %d", getName(), DEFAULT_BAUD_RATE, FAST_BAUD_RATE);
        // The OK still comes at the old rate; the module switches straight after
        auto response = send(set, DEFAULT_TIMEOUT);
        if (!response.ok()) {
            LOGTW(CELLULAR, "%s failed: %s %s; staying at %d baud", set.c_str(), toString(response.result), response.error.c_str(), DEFAULT_BAUD_RATE);
            return;
        }
        setHostBaudRate(FAST_BAUD_RATE);
        for (int attempt = 1; attempt <= BAUD_CONFIRM_ATTEMPTS; attempt++) {
            if (send("AT", WAKE_TIMEOUT).ok()) {
                return;
            }
        }
        LOGTW(CELLULAR, "%s did not answer at %d baud, going back to %d", getName(), FAST_BAUD_RATE, DEFAULT_BAUD_RATE);
        setHostBaudRate(DEFAULT_BAUD_RATE);
        if (send("AT", WAKE_TIMEOUT).ok()) {
            // It never switched
            return;
        }
        // It switched, but the link garbles the fast rate: ask it back, in case enough gets through
        setHostBaudRate(FAST_BAUD_RATE);
        send("AT+IPR=" + std::to_string(DEFAULT_BAUD_RATE), DEFAULT_TIMEOUT);
        setHostBaudRate(DEFAULT_BAUD_RATE);
    }

    void setHostBaudRate(int rate) {
        // Whatever is still going out would be garbled by the switch
        uart_wait_tx_done(port, pdMS_TO_TICKS(100));
        ESP_ERROR_CHECK_WITHOUT_ABORT(uart_set_baudrate(port, static_cast<uint32_t>(rate)));
    }

    /**
     * How the bytes from the module are split between commands and URCs, given how esp_modem
     * buffers them: it hands every callback its whole buffer so far, and empties that buffer only
     * when a command returns. A callback answering TIMEOUT keeps the latest chunk in the buffer,
     * anything else drops it.
     *
     * - Outside of commands, processUrcData() takes every chunk as it comes and drops it from the
     *   buffer, so the buffer doesn't fill up between commands; urcLines keeps a line cut off at
     *   the end of a chunk. In direct push mode one URC is a whole TCP segment as hex, more than
     *   esp_modem's buffer holds.
     * - While a command waits for its response, chunks stay in the buffer for its callback to
     *   parse. Once the response is complete, the callback hands what came with it to the URC
     *   handler: URCs that arrived during the command, and whatever followed the final result
     *   code; esp_modem would drop the latter. Later chunks, until the command returns, go to
     *   processUrcData() again, in order.
     *
     * urcBufferPrefix is what the buffer is known to start with, to tell the new part of a chunk
     * apart from what has been handled already, and to tell when esp_modem has emptied it. The
     * chunk that completes a response is dropped from the buffer too (esp_modem treats it as
     * consumed), so while a command is in flight the buffer holds what the previous chunk left.
     */
    AtResponse send(const std::string& command, milliseconds timeout, std::string_view awaitAfterOk = {}) {
        std::optional<AtResponse> response;
        size_t alreadyHandled;
        // The rest of a line that started before this command, if one did
        bool continuesLine;
        {
            std::scoped_lock lock(urcHandlerMutex);
            alreadyHandled = urcBufferPrefix.size();
            continuesLine = urcLines.hasPending();
            responseComplete = false;
            inFlightBuffer.reset();
            commandInFlight = true;
        }
        dte->command(
            command + "\r",
            [&](uint8_t* data, size_t len) {
                std::string_view whole(reinterpret_cast<const char*>(data), len);
                auto buffer = whole.substr(std::min(alreadyHandled, whole.size()));
                std::string_view continuation;
                if (continuesLine) {
                    auto newline = buffer.find('\n');
                    if (newline == std::string_view::npos) {
                        return esp_modem::command_result::TIMEOUT;
                    }
                    continuation = buffer.substr(0, newline + 1);
                    buffer.remove_prefix(newline + 1);
                }
                size_t consumed = 0;
                response = parseAtResponse(buffer, command, awaitAfterOk, &consumed);
                if (!response) {
                    return esp_modem::command_result::TIMEOUT;
                }
                std::scoped_lock lock(urcHandlerMutex);
                feedUrcData(continuation);
                dispatchUrcsInResponse(*response, command);
                feedUrcData(buffer.substr(consumed));
                responseComplete = true;
                return response->ok() ? esp_modem::command_result::OK : esp_modem::command_result::FAIL;
            },
            static_cast<uint32_t>(timeout.count()));
        {
            std::scoped_lock lock(urcHandlerMutex);
            commandInFlight = false;
            // esp_modem has emptied its buffer by now, and after the response nothing was added
            // to it. Without a response, whatever arrived after the buffer was emptied stays in
            // it, unread
            if (responseComplete) {
                urcBufferPrefix.clear();
            } else if (inFlightBuffer) {
                urcBufferPrefix = std::move(*inFlightBuffer);
            }
            inFlightBuffer.reset();
        }
        if (!response) {
            return AtResponse { .result = AtResult::Timeout, .lines = {}, .error = {} };
        }
        return std::move(*response);
    }

    /**
     * @brief Hands URCs that arrived while the command was in flight to the URC handler, instead
     * of leaving them among the command's response lines where nothing would look at them. Call
     * with urcHandlerMutex held.
     */
    void dispatchUrcsInResponse(AtResponse& response, std::string_view command) {
        auto urcs = std::ranges::stable_partition(response.lines, [&](const std::string& line) {
            return !isUrc(line, command);
        });
        for (const auto& line : urcs) {
            dispatchUrc(line);
        }
        response.lines.erase(urcs.begin(), urcs.end());
    }

    static bool isUrc(std::string_view line, std::string_view command) {
        static constexpr std::array<std::string_view, 8> URC_PREFIXES {
            "+CEREG:", "+QIURC:", "+CTZEU:", "+CTZV:", "+IP:", "+QNBIOTEVENT:", "+CSCON:", "+CEDRXP:"
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

    /**
     * @brief Handles socket URCs here, passes everything else on. Call with urcHandlerMutex held.
     */
    void dispatchUrc(std::string_view line) {
        if (auto event = parseQiurc(line)) {
            if (event->connectId == CONNECT_ID && socketEventHandler) {
                socketEventHandler(*event);
            }
            return;
        }
        if (urcHandler) {
            urcHandler(line);
        }
    }

    /**
     * @brief Splits data from the module into URC lines. Call with urcHandlerMutex held.
     */
    void feedUrcData(std::string_view data) {
        auto overlong = urcLines.getOverlongCount();
        urcLines.feed(data, [this](std::string_view line) {
            dispatchUrc(line);
        });
        if (urcLines.getOverlongCount() != overlong) {
            LOGTW(CELLULAR, "Dropped the rest of a line from the modem longer than %zu bytes", MAX_URC_LINE);
        }
    }

    /**
     * @brief Takes the data that arrives outside of a command's response (see send()).
     */
    esp_modem::command_result processUrcData(std::string_view data) {
        std::scoped_lock lock(urcHandlerMutex);
        if (commandInFlight && !responseComplete) {
            // Stays in the buffer for the command's callback, unless this chunk completes the
            // response; in that case the buffer still holds what the previous chunk left
            if (inFlightBuffer) {
                urcBufferPrefix = std::move(*inFlightBuffer);
            }
            inFlightBuffer.emplace(data);
            return esp_modem::command_result::TIMEOUT;
        }
        if (!urcBufferPrefix.empty()) {
            if (data.starts_with(urcBufferPrefix)) {
                data.remove_prefix(urcBufferPrefix.size());
            } else {
                // esp_modem has emptied its buffer since
                urcBufferPrefix.clear();
            }
        }
        feedUrcData(data);
        // Drops the chunk from the buffer, which keeps starting with urcBufferPrefix
        return esp_modem::command_result::OK;
    }

    bool ensureFullFunctionality() {
        // The module keeps CFUN across restarts, and at 0 (minimum functionality) it doesn't even
        // power the SIM: +CPIN: NOT READY, and no attach. The bring-up module started out that way
        auto response = command("AT+CFUN?", DEFAULT_TIMEOUT);
        auto fields = response.ok() ? findFields(response, "+CFUN:") : std::nullopt;
        if (fields && !fields->empty() && (*fields)[0].asInt() == 1) {
            return true;
        }
        LOGTI(CELLULAR, "Switching %s to full functionality (was: %s)", getName(),
            response.lines.empty() ? "?" : response.lines.front().c_str());
        response = command("AT+CFUN=1", CFUN_TIMEOUT);
        if (!response.ok()) {
            LOGTW(CELLULAR, "AT+CFUN=1 failed: %s %s", toString(response.result), response.error.c_str());
            return false;
        }
        return true;
    }

    bool ensureRxdWakeup() {
        // Without it, the module can't be woken over the UART once it sleeps. Saved to NVRAM, but
        // only takes effect after a restart
        static constexpr PersistedSetting RXD_WAKEUP {
            .query = R"(AT+QCFG="wakeupRXD")", .expected = R"(+QCFG: "wakeupRXD",1)", .set = R"(AT+QCFG="wakeupRXD",1)"
        };
        auto current = checkSetting(RXD_WAKEUP);
        if (!current) {
            return true;
        }
        LOGTI(CELLULAR, "Setting %s (was: %s), and restarting %s for it to take effect", RXD_WAKEUP.set, current->c_str(), getName());
        auto response = command(RXD_WAKEUP.set, DEFAULT_TIMEOUT);
        if (!response.ok()) {
            LOGTW(CELLULAR, "%s failed: %s %s", RXD_WAKEUP.set, toString(response.result), response.error.c_str());
            return false;
        }
        // Answers OK, then resets straight away
        command("AT+QRST=1", DEFAULT_TIMEOUT);
        // Booting takes a moment; waking retries until it answers
        return wake();
    }

    bool ensureReleaseVersion() {
        // Release 14 makes the module Cat NB2 (NBcategory follows it to 2): 2536-bit downlink
        // transport blocks instead of 680, and two HARQ processes, if the network supports it.
        // Saved to NVRAM, only accepted at minimum functionality, and only takes effect after a
        // restart. ensureFullFunctionality() brings CFUN back to 1 afterwards
        static constexpr PersistedSetting RELEASE_VERSION {
            .query = R"(AT+QCFG="relversion")", .expected = R"(+QCFG: "relversion",14)", .set = R"(AT+QCFG="relversion",14)"
        };
        auto current = checkSetting(RELEASE_VERSION);
        if (!current) {
            return true;
        }
        LOGTI(CELLULAR, "Setting %s (was: %s), and restarting %s for it to take effect", RELEASE_VERSION.set, current->c_str(), getName());
        auto response = command("AT+CFUN=0", CFUN_TIMEOUT);
        if (!response.ok()) {
            LOGTW(CELLULAR, "AT+CFUN=0 failed: %s %s", toString(response.result), response.error.c_str());
            return false;
        }
        response = command(RELEASE_VERSION.set, DEFAULT_TIMEOUT);
        if (!response.ok()) {
            LOGTW(CELLULAR, "%s failed: %s %s", RELEASE_VERSION.set, toString(response.result), response.error.c_str());
            return false;
        }
        // Answers OK, then resets straight away
        command("AT+QRST=1", DEFAULT_TIMEOUT);
        // Booting takes a moment; waking retries until it answers
        return wake();
    }

    bool ensureEdrx(std::optional<milliseconds> cycle) {
        // Written on every boot: AT+CEDRXS? shows the requested cycle whether or not eDRX is on,
        // so it can't tell us whether it needs writing. The bench module answered with our cycle
        // and still didn't use eDRX until it was written
        std::string set;
        if (cycle) {
            auto code = encodeEdrxCycle(*cycle);
            if (!code) {
                LOGTW(CELLULAR, "NB-IoT has no %lld ms eDRX cycle, leaving the module's setting as it is",
                    static_cast<long long>(cycle->count()));
                return false;
            }
            // Mode 2 also reports what the network grants as +CEDRXP
            set = R"(AT+CEDRXS=2,5,")" + *code + "\"";
        } else {
            // Mode 3 also discards the requested cycle
            set = "AT+CEDRXS=3";
        }
        LOGTD(CELLULAR, "Requesting eDRX: %s", set.c_str());
        auto response = command(set, DEFAULT_TIMEOUT);
        if (!response.ok()) {
            LOGTW(CELLULAR, "%s failed: %s %s", set.c_str(), toString(response.result), response.error.c_str());
        }
        return response.ok();
    }

    bool ensureBands() {
        // Without a band list the module scans every band it supports before it finds a cell,
        // which can take many minutes. Saved to NVRAM, and a write forces a detach and re-attach,
        // so only write it when it differs.
        // TODO Make this configurable once devices are deployed outside of Europe
        auto response = command("AT+QBAND?", DEFAULT_TIMEOUT);
        auto fields = response.ok() ? findFields(response, "+QBAND:") : std::nullopt;
        std::vector<int> current;
        for (const auto& field : fields.value_or(std::vector<AtField> {})) {
            if (auto band = field.asInt()) {
                current.push_back(*band);
            }
        }
        if (current == std::vector<int>(BANDS.begin(), BANDS.end())) {
            return true;
        }

        std::string bands = std::to_string(BANDS.size());
        for (int band : BANDS) {
            bands += "," + std::to_string(band);
        }
        LOGTI(CELLULAR, "Restricting %s to bands %s (was: %s)", getName(), bands.c_str(),
            response.lines.empty() ? "?" : response.lines.front().c_str());
        std::string setBands = "AT+QBAND=" + bands;
        response = command(setBands, QBAND_TIMEOUT);
        if (!response.ok()) {
            LOGTW(CELLULAR, "%s failed: %s %s", setBands.c_str(), toString(response.result), response.error.c_str());
            return false;
        }
        return true;
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

    void logResponse(const std::string& command, milliseconds timeout = DEFAULT_TIMEOUT) {
        auto response = this->command(command, timeout);
        if (!response.ok()) {
            LOGTW(CELLULAR, "%s: %s %s", command.c_str(), toString(response.result), response.error.c_str());
            return;
        }
        for (const auto& line : response.lines) {
            LOGTI(CELLULAR, "%s: %s", command.c_str(), line.c_str());
        }
    }

    // Most commands answer well within this; the AT manual gives 300 ms to 5 s maximums
    static constexpr milliseconds DEFAULT_TIMEOUT = 5s;
    // Maximum response times from the AT manual
    static constexpr milliseconds QENG_TIMEOUT = 15s;
    static constexpr milliseconds CFUN_TIMEOUT = 25s;
    static constexpr milliseconds QBAND_TIMEOUT = 900s;
    // The application note suggests waiting up to 60 s for the +QIOPEN URC
    static constexpr milliseconds QIOPEN_TIMEOUT = 65s;
    static constexpr milliseconds QICLOSE_TIMEOUT = 10s;
    // 300 ms is the maximum response time, plus about 0.1 s for 1 KB of hex at 115200 baud
    static constexpr milliseconds QIRD_TIMEOUT = 2s;
    // Up to SEND OK, which only means the data has reached the module's TCP stack
    static constexpr milliseconds QISEND_TIMEOUT = 10s;
    static constexpr milliseconds NTP_TIMEOUT = 30s;

    static constexpr int SEND_ATTEMPTS = 3;
    static constexpr milliseconds SEND_RETRY_DELAY = 1s;

    // The one connection we use; the module has five
    static constexpr int CONNECT_ID = 0;

    // The NB-IoT bands used in Europe, in search order: B20 (800 MHz) first, B8 (900 MHz) second
    static constexpr std::array BANDS { 20, 8 };

    // The module's default, and the most it takes (AT+IPR; hardware design guide, "Main UART
    // Interface"). The UART's divisor from the 40 MHz crystal is 86.8125 for 460800, 0.01% off
    static constexpr int DEFAULT_BAUD_RATE = 115200;
    static constexpr int FAST_BAUD_RATE = 460800;
    static constexpr int BAUD_CONFIRM_ATTEMPTS = 3;
    static constexpr milliseconds PROBE_FLUSH_TIMEOUT = 100ms;

    static constexpr int WAKE_ATTEMPTS = 10;
    static constexpr milliseconds WAKE_TIMEOUT = 300ms;
    static constexpr milliseconds WAKE_INITIAL_BACKOFF = 100ms;
    static constexpr milliseconds WAKE_MAX_BACKOFF = 1s;
    // At about 2 KB/s, a command that holds the module this long lets its receive buffer fill
    static constexpr milliseconds SLOW_COMMAND = 1s;
    // How long a pause in pushed data may hold up the next request, a range of the update image
    static constexpr milliseconds MAX_PUSH_PAUSE = 5s;
    static constexpr milliseconds PUSH_PAUSE_GRACE = 500ms;

    const uart_port_t port;
    std::atomic<uint32_t> uartErrors { 0 };

    // Keeps the ESP32 out of light sleep while a command is in flight
    PowerManagementLock noLightSleep { "cellular", ESP_PM_NO_LIGHT_SLEEP };

    // Keeps the wake sequence and the command that follows it together
    std::mutex commandMutex;
    const bool pushSocketData;
    std::optional<PowerManagementLockGuard> awakeWhilePushing;
    // Guards the state below, which holds back commands while a socket pushes data (see
    // awaitPushPause())
    std::mutex pushGateMutex;
    std::condition_variable pushGate;
    // While a socket in direct push mode is open
    bool pushSocketOpen = false;
    bool inPushPause = false;
    int commandsHeldBack = 0;
    int commandsInPause = 0;
    // The last command sent, for the log when another one had to wait for it; only touched with
    // commandMutex held
    std::string lastCommand;

    // Guards the URC handlers, and the state below that splits the bytes from the module between
    // commands and URCs (see send())
    std::mutex urcHandlerMutex;
    UrcHandler urcHandler;
    SocketEventHandler socketEventHandler;
    bool commandInFlight = false;
    // Whether the command in flight has its whole response
    bool responseComplete = false;
    std::string urcBufferPrefix;
    // The buffer as of the latest chunk of the command in flight
    std::optional<std::string> inFlightBuffer;
    // A pushed URC carries up to a TCP segment as hex, about 3 KB
    static constexpr size_t MAX_URC_LINE = 4096;
    AtLineAssembler urcLines { MAX_URC_LINE };
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
