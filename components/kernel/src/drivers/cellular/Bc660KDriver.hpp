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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
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
    explicit Bc660KDriver(const std::shared_ptr<esp_modem::DTE>& dte)
        : GenericModule(dte, std::make_unique<esp_modem::PdpContext>("")) {
        dte->set_urc_cb([this](uint8_t* data, size_t len) {
            return processUrcData(std::string_view(reinterpret_cast<const char*>(data), len));
        });
    }

    const char* getName() const override {
        return "BC660K-GL";
    }

    bool wake() override {
        PowerManagementLockGuard awake(noLightSleep);
        std::scoped_lock lock(commandMutex);
        return wakeLocked();
    }

    bool configure() override {
        // First, since it may restart the module, which loses the settings below
        bool success = ensureRxdWakeup();

        // None of these is ever wrong to repeat, so there's no point checking first
        static constexpr std::array SETTINGS {
            // Echo is on after power-on; off keeps the command out of every response
            "ATE0",
            // A readable "+CME ERROR: <text>" instead of a bare ERROR
            "AT+CMEE=2",
            // Let the module light sleep between paging occasions; it wakes on the network's
            // schedule and on UART activity from us (the first command after that is lost, see
            // wakeLocked()). Not deep sleep: that only happens in PSM, which is off. Not saved to
            // NVRAM, so it has to be sent after every module restart
            "AT+QSCLK=2",
            // Only URC mode 3 carries the EMM reject cause, which is what tells "no coverage"
            // apart from "subscription refused"
            "AT+CEREG=3",
            // Report entering and leaving PSM
            "AT+QNBIOTEVENT=1,1",
            // Report the RRC connection going up and down (+CSCON), for the debug console and
            // for telemetry on how long the radio stays connected
            "AT+CSCON=1",
        };

        for (const char* setting : SETTINGS) {
            auto response = command(setting, DEFAULT_TIMEOUT);
            if (!response.ok()) {
                LOGTW(CELLULAR, "%s failed: %s %s", setting, toString(response.result), response.error.c_str());
                success = false;
            }
        }

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
        return this->command(command, timeout, std::string_view {});
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
            // Local port assigned automatically, buffer access mode
            + ",0,0";
        auto response = command(open, QIOPEN_TIMEOUT, "+QIOPEN:");
        if (!response.ok() || response.lines.empty()) {
            LOGTW(CELLULAR, "Could not connect to %s:%d: %s %s", host.c_str(), port, toString(response.result), response.error.c_str());
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
    }

    bool send(const uint8_t* data, size_t length) override {
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
            return SocketReceive { .length = data->length, .more = more };
        }
        LOGTW(CELLULAR, "%s: no data line in the response", read.c_str());
        return std::nullopt;
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

    /**
     * @param awaitAfterOk see parseAtResponse()
     */
    AtResponse command(const std::string& command, milliseconds timeout, std::string_view awaitAfterOk) {
        // The UART driver only keeps the ESP32 awake while transmitting; in light sleep the
        // response would be lost, as waking on UART edges loses the first bytes
        PowerManagementLockGuard awake(noLightSleep);
        std::scoped_lock lock(commandMutex);
        // The module can be asleep before any command, and the first AT only wakes it
        if (!wakeLocked()) {
            return AtResponse { .result = AtResult::Timeout, .lines = {}, .error = {} };
        }
        return send(command, timeout, awaitAfterOk);
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

    AtResponse send(const std::string& command, milliseconds timeout, std::string_view awaitAfterOk = {}) {
        std::optional<AtResponse> response;
        commandInFlight = true;
        // The buffer handed to the callback still starts with whatever arrived since the last
        // command, including URCs processUrcData() has already handled; skip those, or they
        // would be handled again as URCs that arrived during this command
        size_t alreadyHandled = urcBytesHandled;
        dte->command(
            command + "\r",
            [&](uint8_t* data, size_t len) {
                std::string_view buffer(reinterpret_cast<const char*>(data), len);
                buffer.remove_prefix(std::min(alreadyHandled, buffer.size()));
                response = parseAtResponse(buffer, command, awaitAfterOk);
                if (!response) {
                    return esp_modem::command_result::TIMEOUT;
                }
                return response->ok() ? esp_modem::command_result::OK : esp_modem::command_result::FAIL;
            },
            static_cast<uint32_t>(timeout.count()));
        // esp_modem empties its receive buffer when a command completes
        urcBytesHandled = 0;
        commandInFlight = false;
        if (!response) {
            return AtResponse { .result = AtResult::Timeout, .lines = {}, .error = {} };
        }
        dispatchUrcsInResponse(*response, command);
        return std::move(*response);
    }

    /**
     * @brief Hands URCs that arrived while the command was in flight to the URC handler, instead
     * of leaving them among the command's response lines where nothing would look at them.
     */
    void dispatchUrcsInResponse(AtResponse& response, std::string_view command) {
        auto urcs = std::ranges::stable_partition(response.lines, [&](const std::string& line) {
            return !isUrc(line, command);
        });
        if (urcs.empty()) {
            return;
        }
        std::vector<std::string> lines(std::make_move_iterator(urcs.begin()), std::make_move_iterator(urcs.end()));
        response.lines.erase(urcs.begin(), urcs.end());
        std::scoped_lock lock(urcHandlerMutex);
        for (const auto& line : lines) {
            dispatchUrc(line);
        }
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
                socketEventHandler(event->type);
            }
            return;
        }
        if (urcHandler) {
            urcHandler(line);
        }
    }

    /**
     * @brief Splits data arriving outside of a command into URC lines.
     *
     * esp_modem hands this callback everything it has received since its buffer was last
     * emptied, which only happens when a command completes. So the data can start with lines
     * handled on an earlier call; urcBytesHandled tracks where the new ones begin. Always
     * answers TIMEOUT, which keeps the data in the buffer: "consuming" it here would only drop
     * the latest chunk and leave the rest, so offsets would no longer line up.
     *
     * While a command is in flight the same data belongs to the command, so leave it alone.
     */
    esp_modem::command_result processUrcData(std::string_view data) {
        if (commandInFlight) {
            return esp_modem::command_result::TIMEOUT;
        }
        data.remove_prefix(std::min<size_t>(urcBytesHandled, data.size()));
        auto lastNewline = data.rfind('\n');
        if (lastNewline == std::string_view::npos) {
            return esp_modem::command_result::TIMEOUT;
        }
        auto complete = data.substr(0, lastNewline + 1);
        urcBytesHandled += complete.size();

        std::scoped_lock lock(urcHandlerMutex);
        while (!complete.empty()) {
            auto newline = complete.find('\n');
            std::string_view line = complete.substr(0, newline);
            complete.remove_prefix(newline + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                line.remove_suffix(1);
            }
            if (!line.empty()) {
                dispatchUrc(line);
            }
        }
        return esp_modem::command_result::TIMEOUT;
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

    static constexpr int WAKE_ATTEMPTS = 10;
    static constexpr milliseconds WAKE_TIMEOUT = 300ms;
    static constexpr milliseconds WAKE_INITIAL_BACKOFF = 100ms;
    static constexpr milliseconds WAKE_MAX_BACKOFF = 1s;

    // Keeps the ESP32 out of light sleep while a command is in flight
    PowerManagementLock noLightSleep { "cellular", ESP_PM_NO_LIGHT_SLEEP };

    // Keeps the wake sequence and the command that follows it together
    std::mutex commandMutex;
    std::atomic<bool> commandInFlight { false };
    std::atomic<size_t> urcBytesHandled { 0 };

    std::mutex urcHandlerMutex;
    UrcHandler urcHandler;
    SocketEventHandler socketEventHandler;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
