#pragma once

#include <Log.hpp>
#include <PowerManager.hpp>
#include <Queue.hpp>
#include <State.hpp>
#include <Task.hpp>
#include <drivers/cellular/AtSocketTransport.hpp>
#include <drivers/cellular/Bc660KDriver.hpp>
#include <drivers/cellular/CellularConfig.hpp>
#include <drivers/cellular/CellularModemPins.hpp>
#include <drivers/cellular/CellularModuleDriver.hpp>
#include <drivers/cellular/Cereg.hpp>
#include <drivers/cellular/Edrx.hpp>
#include <drivers/cellular/EspModem.hpp>
#include <drivers/cellular/NetworkTime.hpp>
#include <drivers/cellular/RadioStatus.hpp>

#include <ArduinoJson.h>
#include <sdkconfig.h>
#include <soc/uart_pins.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace std::chrono_literals;

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

/**
 * @brief What CellularDriver last saw of the link, for status displays.
 */
struct CellularStatus {
    // Whether the modem is up and answering at all
    bool modemUp = false;
    std::optional<RegistrationStatus> registration;
    // Set once the network is ready
    std::optional<std::string> ipAddress;
    // Only while camped on a cell
    std::optional<ServingCell> servingCell;
    // Whether the radio has an RRC connection to the cell (true) or is idle (false)
    std::optional<bool> rrcConnected;
};

/**
 * @brief Owns the cellular modem: its UART, bring-up, registration, network time, and the socket
 * transport MQTT runs over (docs/specs/NB-IoT.md).
 *
 * Reports the network the same way WiFiDriver does, through the networkConnecting and
 * networkReady states, so nothing downstream needs to know which link is in use. Everything
 * chipset-specific goes through CellularModuleDriver.
 *
 * The modem is optional hardware, so nothing here is fatal: if the modem's pins are taken by the
 * console, or it does not answer, the driver logs why and the device carries on without it.
 */
class CellularDriver {
public:
    // Called with UTC time from the network (NITZ) or the module's NTP client, and which it was
    using TimeHandler = std::function<void(time_t utcTime, const char* source)>;

    CellularDriver(
        const CellularModemPins& pins,
        const std::shared_ptr<CellularConfig>& config,
        StateSource& networkConnecting,
        StateSource& networkReady,
        const State& rtcInSync,
        std::string ntpServer,
        TimeHandler onNetworkTime)
        : pins(pins)
        , edrxCycle(toEdrxCycle(config->edrxCycle.get()))
        , networkConnecting(networkConnecting)
        , networkReady(networkReady)
        , rtcInSync(rtcInSync)
        , ntpServer(std::move(ntpServer))
        , onNetworkTime(std::move(onNetworkTime)) {
        Task::run("cellular", 4096, [this](Task& /*task*/) {
            try {
                run();
            } catch (const std::exception& e) {
                LOGTE(CELLULAR, "Cellular modem failed: %s", e.what());
                // Don't keep the device out of light sleep for a modem we've given up on
                noLightSleepGuard.reset();
            }
            // Only gets here when the modem is out of the picture
            this->networkConnecting.clear();
        });
    }

    /**
     * @brief The transport for esp-mqtt; it can't connect until the modem has a network.
     */
    esp_transport_handle_t getTransport() const {
        return transport.getHandle();
    }

    /**
     * @brief The link as of the last registration check, without talking to the modem.
     */
    CellularStatus getStatus() const {
        std::scoped_lock lock(statusMutex);
        return status;
    }

    /**
     * @brief The link quality as of the last registration check, and the bytes sent and
     * received over the modem since the last call (docs/specs/NB-IoT.md, "Link quality in
     * telemetry").
     */
    void populateTelemetry(JsonObject& json) {
        auto cell = getStatus().servingCell;
        if (cell) {
            json["cell"] = cell->cellId;
            json["band"] = cell->band;
            auto set = [&](const char* name, std::optional<int> value) {
                if (value) {
                    json[name] = *value;
                }
            };
            set("rsrp", cell->rsrp);
            set("rsrq", cell->rsrq);
            set("sinr", cell->sinr);
            set("ecl", cell->ecl);
        }
        auto [sent, received] = transport.takeTrafficCounts();
        json["bytes-sent"] = sent;
        json["bytes-received"] = received;
        json["rrc-connected-ms"] = takeRrcConnectedTime().count();
    }

private:
    void run() {
        if (sharesPinsWithConsole(pins)) {
            // UD_UART0_CONSOLE builds put the console back on these pins for early-boot debugging
            LOGTW(CELLULAR, "Console is on UART0, which uses the modem's pins (GPIO %d/%d); not starting the modem",
                static_cast<int>(pins.tx->getGpio()), static_cast<int>(pins.rx->getGpio()));
            return;
        }

        networkConnecting.set();
        LOGTI(CELLULAR, "Starting modem on UART%d (TX GPIO %d, RX GPIO %d)",
            static_cast<int>(MODEM_UART), static_cast<int>(pins.tx->getGpio()), static_cast<int>(pins.rx->getGpio()));

        // The UART driver only holds its PM lock while transmitting, so in light sleep anything the
        // modem sends -- the reply to a command we are waiting on, or a URC -- would be lost. Stay
        // awake for as long as the modem is up; waking on UART activity instead belongs with
        // putting the modem itself to sleep (docs/specs/NB-IoT.md, stage 4).
        noLightSleepGuard = std::make_unique<PowerManagementLockGuard>(noLightSleep);

        auto dte = createDte(pins);
        module = std::make_shared<Bc660KDriver>(dte);
        module->onUrc([this](std::string_view line) {
            handleUrc(line);
        });

        if (!module->wake()) {
            LOGTE(CELLULAR, "%s is not answering; is the daughter board connected?", module->getName());
            noLightSleepGuard.reset();
            return;
        }
        LOGTI(CELLULAR, "%s is answering", module->getName());

        if (!module->configure()) {
            LOGTW(CELLULAR, "Some %s settings could not be applied", module->getName());
        }
        if (!module->configurePowerSaving(edrxCycle)) {
            LOGTW(CELLULAR, "Could not configure %s power saving", module->getName());
        }
        module->logStatus();
        updateStatus([](CellularStatus& status) { status.modemUp = true; });
        transport.attach(module);
        if (rtcInSync.isSet()) {
            // Kept across a soft reset; still worth a fresh sync eventually
            lastTimeSync = steady_clock::now();
        }
        monitorNetwork();
    }

    /**
     * @brief Follows registration, and reports the network ready once there is an IP address.
     *
     * A +CEREG URC triggers a fresh look straight away; on top of that registration is polled,
     * since the URCs only report changes: a modem that keeps searching would never say so.
     */
    [[noreturn]] void monitorNetwork() {
        bool wasRegistered = false;
        std::optional<RegistrationStatus> lastStatus;
        while (true) {
            auto registration = queryRegistration();
            bool registered = registration && registration->isRegistered();
            if (!registration) {
                LOGTW(CELLULAR, "Could not read the registration status");
            } else if (!registered || registration->status != lastStatus) {
                LOGTI(CELLULAR, "Network: %s", describe(*registration).c_str());
            }
            if (registered && !wasRegistered) {
                logNetworkDetails();
            }
            // Not logged once registered; read anyway to keep the status current
            auto cell = !registered || !wasRegistered ? module->logRadioStatus() : module->queryServingCell();
            // The URC only reports changes, and could have been missed
            if (auto rrcConnected = module->queryRrcConnected()) {
                updateRrcState(*rrcConnected);
            }
            if (registered) {
                logPagingIfChanged();
            }
            updateStatus([&](CellularStatus& status) {
                status.registration = registration ? std::optional(registration->status) : std::nullopt;
                status.servingCell = cell && cell->isCamped() ? cell : std::nullopt;
            });
            if (!registered) {
                // Automatic or manual operator selection, and the operator if there is one
                logQuery("AT+COPS?");
            }
            updateNetworkState(registration, cell);
            syncTimeIfDue();

            wasRegistered = registered;
            lastStatus = registration ? std::optional(registration->status) : std::nullopt;
            bool settled = networkReady.isSet() && rtcInSync.isSet();
            networkChanged.pollIn(ticks(settled ? REGISTERED_POLL_INTERVAL : SEARCHING_POLL_INTERVAL));
        }
    }

    /**
     * @brief Reports the network ready once registered with an IP address, and not ready again
     * when registration is lost.
     */
    void updateNetworkState(const std::optional<Registration>& registration, const std::optional<ServingCell>& cell) {
        bool registered = registration && registration->isRegistered();
        if (registered && !networkReady.isSet()) {
            // Registered isn't enough to open connections: the PDP context needs an address too
            auto address = module->queryIpAddress();
            if (!address) {
                LOGTI(CELLULAR, "Registered, waiting for an IP address");
                return;
            }
            module->prepareNetwork();
            LOGTI(CELLULAR, "Network ready, IP address %s, %s; %s", address->c_str(),
                describe(*registration).c_str(), cell ? describe(*cell).c_str() : "no serving cell info");
            networkReadySince = steady_clock::now();
            updateStatus([&](CellularStatus& status) { status.ipAddress = address; });
            networkConnecting.clear();
            networkReady.set();
        } else if (!registered && networkReady.isSet()) {
            LOGTW(CELLULAR, "Lost the network");
            updateStatus([](CellularStatus& status) { status.ipAddress.reset(); });
            networkReady.clear();
            networkConnecting.set();
        }
    }

    /**
     * @brief Falls back to NTP when the network doesn't send its time (NITZ is optional for
     * operators), and re-syncs once a day, since NITZ only comes with an attach.
     */
    void syncTimeIfDue() {
        if (!networkReady.isSet()) {
            return;
        }
        auto now = steady_clock::now();
        bool due;
        if (rtcInSync.isSet()) {
            due = now - lastTimeSync.load() >= TIME_RESYNC_INTERVAL;
        } else {
            due = now - networkReadySince >= NITZ_GRACE_PERIOD
                && (!lastNtpAttempt || now - *lastNtpAttempt >= NTP_RETRY_INTERVAL);
        }
        if (!due) {
            return;
        }
        lastNtpAttempt = now;
        if (!rtcInSync.isSet()) {
            LOGTI(CELLULAR, "No time from the network, asking %s", ntpServer.c_str());
        }
        if (auto time = module->queryNtpTime(ntpServer)) {
            lastTimeSync = steady_clock::now();
            onNetworkTime(*time, "NTP via modem");
        }
    }

    /**
     * @brief Changes the cached status for getStatus() under its lock.
     */
    template <typename Update>
    void updateStatus(Update update) {
        std::scoped_lock lock(statusMutex);
        update(status);
    }

    std::optional<Registration> queryRegistration() {
        auto response = module->command("AT+CEREG?", 5s);
        for (const auto& line : response.lines) {
            if (auto registration = parseCeregRead(line)) {
                return registration;
            }
        }
        return std::nullopt;
    }

    void logNetworkDetails() {
        // Operator, packet-domain attach, and the PDP context the network handed out (APN, IP)
        for (const char* query : { "AT+COPS?", "AT+CGATT?", "AT+CGDCONT?" }) {
            logQuery(query);
        }
    }

    /**
     * @brief Logs how often the modem listens for paging, which bounds how long a command takes
     * to arrive, when that changes: what the network made of the eDRX cycle we asked for, and the
     * cell's own paging cycle, which applies without eDRX.
     *
     * Read on every registration check rather than once: the network grants eDRX with an attach
     * or a tracking area update, which can still be in progress right after registering. The
     * paging cycle can only be read while RRC idle.
     */
    void logPagingIfChanged() {
        auto edrx = module->queryEdrx();
        if (edrx && edrx != lastEdrx) {
            LOGTI(CELLULAR, "eDRX: %s", describe(*edrx).c_str());
            lastEdrx = edrx;
        }
        auto cycle = module->queryIdlePagingCycle();
        if (cycle && cycle != lastIdlePagingCycle) {
            LOGTI(CELLULAR, "Paging cycle while idle: %lld ms", static_cast<long long>(cycle->count()));
            lastIdlePagingCycle = cycle;
        }
    }

    /**
     * @brief Tracks the RRC state for the status and for the time spent connected, which is
     * where most of the modem's energy goes. Called from the URC handler too.
     */
    void updateRrcState(bool connected) {
        std::scoped_lock lock(statusMutex);
        if (status.rrcConnected == connected) {
            return;
        }
        auto now = steady_clock::now();
        if (connected) {
            rrcConnectedSince = now;
        } else if (status.rrcConnected == true) {
            rrcConnectedTime += duration_cast<milliseconds>(now - rrcConnectedSince);
        }
        status.rrcConnected = connected;
    }

    /**
     * @brief Time spent in RRC connected state since the last call.
     */
    milliseconds takeRrcConnectedTime() {
        std::scoped_lock lock(statusMutex);
        auto total = rrcConnectedTime;
        if (status.rrcConnected == true) {
            auto now = steady_clock::now();
            total += duration_cast<milliseconds>(now - rrcConnectedSince);
            rrcConnectedSince = now;
        }
        rrcConnectedTime = 0ms;
        return total;
    }

    static std::optional<milliseconds> toEdrxCycle(milliseconds configured) {
        return configured > 0ms ? std::optional(configured) : std::nullopt;
    }

    void logQuery(const char* query) {
        auto response = module->command(query, 5s);
        if (!response.ok()) {
            LOGTW(CELLULAR, "%s: %s %s", query, toString(response.result), response.error.c_str());
            return;
        }
        for (const auto& line : response.lines) {
            LOGTI(CELLULAR, "%s: %s", query, line.c_str());
        }
    }

    static std::shared_ptr<esp_modem::DTE> createDte(const CellularModemPins& pins) {
        esp_modem_dte_config_t config = ESP_MODEM_DTE_DEFAULT_CONFIG();
        config.uart_config.port_num = MODEM_UART;
        config.uart_config.tx_io_num = pins.tx->getGpio();
        config.uart_config.rx_io_num = pins.rx->getGpio();
        config.uart_config.rts_io_num = UART_PIN_NO_CHANGE;
        config.uart_config.cts_io_num = UART_PIN_NO_CHANGE;
        // 115200 is far above what NB-IoT delivers (docs/specs/NB-IoT.md, "UART baud rate")
        config.uart_config.baud_rate = 115200;
        // Room for a full AT+QIRD response: 512 bytes of data as 1024 hex digits, plus framing
        config.dte_buffer_size = 2048;
        auto dte = esp_modem::create_uart_dte(&config);
        if (dte == nullptr) {
            throw std::runtime_error("could not create UART terminal");
        }
        return dte;
    }

    // Runs on the UART's receive task, so it must not send commands to the module
    void handleUrc(std::string_view line) {
        if (auto registration = parseCeregUrc(line)) {
            LOGTI(CELLULAR, "Network: %s", describe(*registration).c_str());
            networkChanged.overwrite(true);
            return;
        }
        if (line.starts_with("+CTZEU:")) {
            if (auto time = parseCtzeu(line)) {
                // Worth knowing whether the operator sends it at all (cornucopia-app#507)
                LOGTI(CELLULAR, "Network time (NITZ): %.*s", static_cast<int>(line.size()), line.data());
                lastTimeSync = steady_clock::now();
                onNetworkTime(*time, "NITZ");
            } else {
                LOGTD(CELLULAR, "Time zone from the network, without the time: %.*s", static_cast<int>(line.size()), line.data());
            }
            return;
        }
        if (auto connected = parseCsconUrc(line)) {
            // Verbose only: a published log record is an uplink, so if this got published,
            // logging "idle" would bring the radio straight back to connected
            LOGTV(CELLULAR, "RRC %s", *connected ? "connected" : "idle");
            updateRrcState(*connected);
            return;
        }
        if (auto edrx = parseCedrxp(line)) {
            LOGTI(CELLULAR, "eDRX changed by the network: %s", describe(*edrx).c_str());
            return;
        }
        if (line.starts_with("+IP:")) {
            // The PDP context got its address
            LOGTD(CELLULAR, "URC: %.*s", static_cast<int>(line.size()), line.data());
            networkChanged.overwrite(true);
            return;
        }
        LOGTD(CELLULAR, "URC: %.*s", static_cast<int>(line.size()), line.data());
    }

    static bool sharesPinsWithConsole([[maybe_unused]] const CellularModemPins& pins) {
#if defined(CONFIG_ESP_CONSOLE_UART) && CONFIG_ESP_CONSOLE_UART_NUM == 0
        auto isConsolePin = [](const InternalPinPtr& pin) {
            return pin->getGpio() == U0TXD_GPIO_NUM || pin->getGpio() == U0RXD_GPIO_NUM;
        };
        return isConsolePin(pins.tx) || isConsolePin(pins.rx);
#else
        return false;
#endif
    }

    static constexpr milliseconds SEARCHING_POLL_INTERVAL = 30s;
    static constexpr milliseconds REGISTERED_POLL_INTERVAL = 5min;

    // How long to wait for NITZ after the network comes up before asking NTP
    static constexpr milliseconds NITZ_GRACE_PERIOD = 30s;
    static constexpr milliseconds NTP_RETRY_INTERVAL = 5min;
    static constexpr milliseconds TIME_RESYNC_INTERVAL = 24h;

    // UART0 is the console's when UD_UART0_CONSOLE is set; UART1 works on any pin via the GPIO matrix
    static constexpr uart_port_t MODEM_UART = UART_NUM_1;

    const CellularModemPins pins;
    // nullopt for eDRX off
    const std::optional<milliseconds> edrxCycle;
    StateSource& networkConnecting;
    StateSource& networkReady;
    const State& rtcInSync;
    const std::string ntpServer;
    const TimeHandler onNetworkTime;

    std::shared_ptr<CellularModuleDriver> module;
    AtSocketTransport transport;

    // Cuts the wait between registration checks short
    CopyQueue<bool> networkChanged { "cellular-network", 1 };
    steady_clock::time_point networkReadySince;
    std::optional<steady_clock::time_point> lastNtpAttempt;
    std::optional<EdrxParameters> lastEdrx;
    std::optional<milliseconds> lastIdlePagingCycle;
    // Set from the URC handler too
    std::atomic<steady_clock::time_point> lastTimeSync;

    mutable std::mutex statusMutex;
    CellularStatus status;
    // Guarded by statusMutex too
    steady_clock::time_point rrcConnectedSince;
    milliseconds rrcConnectedTime { 0 };

    PowerManagementLock noLightSleep { "cellular", ESP_PM_NO_LIGHT_SLEEP };
    std::unique_ptr<PowerManagementLockGuard> noLightSleepGuard;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
