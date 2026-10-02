#pragma once

#include <Log.hpp>
#include <Task.hpp>
#include <drivers/cellular/AtResponse.hpp>
#include <drivers/cellular/CellularModuleDriver.hpp>
#include <drivers/cellular/Cereg.hpp>
#include <drivers/cellular/EspModem.hpp>
#include <drivers/cellular/RadioStatus.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
 * Derives from esp_modem's GenericModule so it can back an esp_modem DCE for the AT socket
 * transport later on (docs/specs/NB-IoT.md, stage 3). The commands here go through command(),
 * which parses the whole response itself rather than looking for "OK" anywhere in it.
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
        std::scoped_lock lock(commandMutex);
        return wakeLocked();
    }

    bool configure() override {
        // None of these is ever wrong to repeat, so there's no point checking first
        static constexpr std::array SETTINGS {
            // Echo is on after power-on; off keeps the command out of every response
            "ATE0",
            // A readable "+CME ERROR: <text>" instead of a bare ERROR
            "AT+CMEE=2",
            // Keep the module out of light and deep sleep for now; sleep comes with eDRX and PSM
            // (stage 4). Not saved to NVRAM, so it has to be sent after every module restart
            "AT+QSCLK=0",
            // Only URC mode 3 carries the EMM reject cause, which is what tells "no coverage"
            // apart from "subscription refused"
            "AT+CEREG=3",
            // Report entering and leaving PSM
            "AT+QNBIOTEVENT=1,1",
        };

        bool success = true;
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
        return success;
    }

    AtResponse command(const std::string& command, milliseconds timeout) override {
        std::scoped_lock lock(commandMutex);
        // The module can be asleep before any command, and the first AT only wakes it
        if (!wakeLocked()) {
            return AtResponse { .result = AtResult::Timeout, .lines = {}, .error = {} };
        }
        return send(command, timeout);
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

    void logRadioStatus() override {
        auto quality = findDecoded(command("AT+CSQ", DEFAULT_TIMEOUT), parseCsq);
        // Radio state of the serving cell. Numeric mode only: the "servingcell" form belongs to
        // Quectel's LTE modules and returns ERROR here
        auto cell = findDecoded(command("AT+QENG=0", QENG_TIMEOUT), parseQengServingCell);

        std::string rssi = quality && quality->rssiDbm ? std::to_string(*quality->rssiDbm) + " dBm" : "unknown";
        std::string serving = cell ? describe(*cell) : "no AT+QENG=0 answer";
        LOGTI(CELLULAR, "Radio: RSSI %s, %s", rssi.c_str(), serving.c_str());

        // EMM state, PLMN search state, and which network it is trying: tells "nothing found"
        // apart from "found an operator that turns us away"
        logResponse("AT+QENG=3", QENG_TIMEOUT);
    }

private:
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

    AtResponse send(const std::string& command, milliseconds timeout) {
        std::optional<AtResponse> response;
        commandInFlight = true;
        dte->command(
            command + "\r",
            [&](uint8_t* data, size_t len) {
                response = parseAtResponse(std::string_view(reinterpret_cast<const char*>(data), len), command);
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
        return std::move(*response);
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
            if (!line.empty() && urcHandler) {
                urcHandler(line);
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

    // The NB-IoT bands used in Europe, in search order: B20 (800 MHz) first, B8 (900 MHz) second
    static constexpr std::array BANDS { 20, 8 };

    static constexpr int WAKE_ATTEMPTS = 10;
    static constexpr milliseconds WAKE_TIMEOUT = 300ms;
    static constexpr milliseconds WAKE_INITIAL_BACKOFF = 100ms;
    static constexpr milliseconds WAKE_MAX_BACKOFF = 1s;

    // Keeps the wake sequence and the command that follows it together
    std::mutex commandMutex;
    std::atomic<bool> commandInFlight { false };
    std::atomic<size_t> urcBytesHandled { 0 };

    std::mutex urcHandlerMutex;
    UrcHandler urcHandler;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
