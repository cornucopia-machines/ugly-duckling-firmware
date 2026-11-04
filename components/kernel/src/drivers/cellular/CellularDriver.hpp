#pragma once

#include <EspError.hpp>
#include <Log.hpp>
#include <Queue.hpp>
#include <Result.hpp>
#include <State.hpp>
#include <Task.hpp>
#include <drivers/cellular/AtSocketTransport.hpp>
#include <drivers/cellular/CellularConfig.hpp>
#include <drivers/cellular/CellularModemPins.hpp>
#include <drivers/cellular/CellularModuleDriver.hpp>
#include <drivers/cellular/Cereg.hpp>
#include <drivers/cellular/Edrx.hpp>
#include <drivers/cellular/NetworkTime.hpp>
#include <drivers/cellular/Nrf9151Driver.hpp>
#include <drivers/cellular/RadioStatus.hpp>
#include <drivers/cellular/Rat.hpp>

#include <ArduinoJson.h>
#include <driver/uart.h>
#include <sdkconfig.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
 * @brief Owns the cellular modem: its bring-up, registration, network time, and the socket
 * transports MQTT and firmware downloads run over (docs/Connectivity.md, "Cellular").
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
    // Called with UTC time from the network (NITZ) or NTP, and which it was
    using TimeHandler = std::function<void(time_t utcTime, const char* source)>;

    CellularDriver(
        const CellularModemPins& pins,
        const std::shared_ptr<CellularConfig>& config,
        StateSource& networkConnecting,
        StateSource& networkReady,
        const State& rtcInSync,
        std::string ntpServer,
        TimeHandler onNetworkTime,
        bool allowModuleSleep)
        : pins(pins)
        , rats(parseRats(config->rats.get()))
        , edrxCycle(toEdrxCycle(config->edrxCycle.get()))
        , allowModuleSleep(allowModuleSleep)
        , networkConnecting(networkConnecting)
        , networkReady(networkReady)
        , rtcInSync(rtcInSync)
        , ntpServer(std::move(ntpServer))
        , onNetworkTime(std::move(onNetworkTime)) {
        Task::run("cellular", 4096, [this](Task& /*task*/) {
            auto result = run();
            if (!result) {
                LOGTE(CELLULAR, "Cellular modem failed: %s", result.error().message.c_str());
            }
            // Only gets here when the modem is out of the picture
            this->networkConnecting.clear();
        });
    }

    /**
     * @brief The transport for esp-mqtt; it can't connect until the modem has a network.
     */
    esp_transport_handle_t getMqttTransport() const {
        return mqttTransport.getHandle();
    }

    /**
     * @brief The transport for firmware downloads, on a connection of its own, so that MQTT's
     * stays up meanwhile.
     */
    esp_transport_handle_t getDownloadTransport() const {
        return downloadTransport.getHandle();
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
     * received over the modem since the last call (docs/specs/abandoned/NB-IoT.md, "Link quality in
     * telemetry").
     */
    void populateTelemetry(JsonObject& json) {
        auto cell = getStatus().servingCell;
        if (cell) {
            json["cell"] = cell->cellId;
            json["band"] = cell->band;
            if (cell->rat) {
                json["rat"] = toConfigName(*cell->rat);
            }
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
        auto [mqttSent, mqttReceived] = mqttTransport.takeTrafficCounts();
        auto [downloadSent, downloadReceived] = downloadTransport.takeTrafficCounts();
        json["bytes-sent"] = mqttSent + downloadSent;
        json["bytes-received"] = mqttReceived + downloadReceived;
        json["rrc-releases"] = mqttTransport.takeReleaseRequests() + downloadTransport.takeReleaseRequests();
        populateRrcTelemetry(json);
    }

private:
    /**
     * @brief Runs the modem; only returns when the modem is out of the picture.
     */
    Result<> run() {
        if (sharesPinsWithConsole(pins)) {
            // UD_UART0_CONSOLE builds put the console back on these pins for early-boot debugging
            LOGTW(CELLULAR, "Console is on UART0, which uses the modem's pins (GPIO %d/%d); not starting the modem",
                static_cast<int>(pins.tx->getGpio()), static_cast<int>(pins.rx->getGpio()));
            return {};
        }

        networkConnecting.set();
        LOGTI(CELLULAR, "Starting modem on UART%d (TX GPIO %d, RX GPIO %d)",
            static_cast<int>(MODEM_UART), static_cast<int>(pins.tx->getGpio()), static_cast<int>(pins.rx->getGpio()));

        module = TRY(Nrf9151Driver::create(MODEM_UART, pins, rats));
        module->onUrc([this](std::string_view line) {
            handleUrc(line);
        });

        if (!module->start()) {
            LOGTE(CELLULAR, "%s is not answering; is Desert Lark connected?", module->getName());
            return {};
        }
        LOGTI(CELLULAR, "%s is answering", module->getName());

        if (!module->configure(allowModuleSleep)) {
            LOGTW(CELLULAR, "Some %s settings could not be applied", module->getName());
        }
        if (!module->configurePowerSaving(edrxCycle)) {
            LOGTW(CELLULAR, "Could not configure %s power saving", module->getName());
        }
        module->logStatus();
        updateStatus([](CellularStatus& status) { status.modemUp = true; });
        mqttTransport.attach(module);
        downloadTransport.attach(module);
        if (rtcInSync.isSet()) {
            // Kept across a soft reset; still worth a fresh sync eventually
            lastTimeSync = steady_clock::now();
        } else if (auto time = module->queryClock()) {
            // An ESP32 restart leaves the modem attached, so no network time comes this boot, but
            // the modem still has it from before (or from an NTP sync written back to it). Works
            // before the network is up, too
            lastTimeSync = steady_clock::now();
            onNetworkTime(*time, "modem clock");
        }
        monitorNetwork();
        return {};
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
                updateRrcState(*rrcConnected, RrcSource::Poll);
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
            auto wait = settled ? REGISTERED_POLL_INTERVAL : SEARCHING_POLL_INTERVAL;
            // Ask NTP as soon as it's due, not on the next registration check after
            if (auto ntpDue = timeUntilNtpDue()) {
                wait = std::clamp(*ntpDue, MIN_POLL_INTERVAL, wait);
            }
            networkChanged.pollIn(ticks(wait));
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
        auto due = timeUntilNtpDue();
        if (!due || *due > 0ms) {
            return;
        }
        lastNtpAttempt = steady_clock::now();
        if (!rtcInSync.isSet()) {
            LOGTI(CELLULAR, "No time from the network, asking %s", ntpServer.c_str());
        }
        if (auto time = module->queryNtpTime(ntpServer)) {
            lastTimeSync = steady_clock::now();
            onNetworkTime(*time, "NTP via modem");
            // For queryClock() after the next ESP32 restart
            if (!module->setClock(*time)) {
                LOGTD(CELLULAR, "Could not set the modem's clock");
            }
        }
    }

    /**
     * @brief How long until NTP is due: the network's time hasn't come within NITZ_GRACE_PERIOD,
     * the last attempt failed NTP_RETRY_INTERVAL ago, or the daily re-sync.
     *
     * @return nullopt while there's no network to ask over; zero or less when due
     */
    std::optional<milliseconds> timeUntilNtpDue() const {
        if (!networkReady.isSet()) {
            return std::nullopt;
        }
        steady_clock::time_point dueAt;
        if (rtcInSync.isSet()) {
            dueAt = lastTimeSync.load() + TIME_RESYNC_INTERVAL;
        } else if (lastNtpAttempt) {
            dueAt = std::max(networkReadySince + NITZ_GRACE_PERIOD, *lastNtpAttempt + NTP_RETRY_INTERVAL);
        } else {
            dueAt = networkReadySince + NITZ_GRACE_PERIOD;
        }
        return duration_cast<milliseconds>(dueAt - steady_clock::now());
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
     * to arrive, when that changes: what the network made of the eDRX cycle we asked for. Without
     * eDRX, the cell's own paging cycle applies (1.28 to 10.24 s), which the modem doesn't report.
     *
     * Read on every registration check rather than once: the network grants eDRX with an attach
     * or a tracking area update, which can still be in progress right after registering.
     */
    void logPagingIfChanged() {
        auto edrx = module->queryEdrx();
        if (edrx && edrx != lastEdrx) {
            LOGTI(CELLULAR, "eDRX: %s", describe(*edrx).c_str());
            lastEdrx = edrx;
        }
    }

    enum class RrcSource : uint8_t {
        // The +CSCON URC, sent by the module as the state changes
        Urc,
        // AT+CSCON?, read on every registration check
        Poll,
    };

    /**
     * @brief Tracks the RRC state for the status, and how long and how often the radio is
     * connected, which is where most of the modem's energy goes. Called from the URC handler too.
     */
    void updateRrcState(bool connected, RrcSource source) {
        std::scoped_lock lock(statusMutex);
        if (status.rrcConnected == connected) {
            return;
        }
        // The URCs should have reported this already: until the read caught up, the time was
        // counted in the wrong state, so the telemetry is only as good as this count is low
        if (source == RrcSource::Poll && status.rrcConnected.has_value()) {
            LOGTV(CELLULAR, "RRC %s, missed by the URCs", connected ? "connected" : "idle");
            rrcMissedChanges++;
        }
        auto now = steady_clock::now();
        if (connected) {
            rrcConnectedSince = now;
            rrcStretchStart = now;
            rrcConnections++;
        } else if (status.rrcConnected == true) {
            rrcConnectedTime += now - rrcConnectedSince;
            rrcLongestConnected = std::max(rrcLongestConnected, now - rrcStretchStart);
        }
        status.rrcConnected = connected;
    }

    /**
     * @brief The share of the time since the last call the radio spent RRC idle, and how many
     * times it connected, the same way PowerManager reports the ESP32's light sleep. Also the
     * longest connection, to tell one connection held open apart from many short ones, and how
     * many changes only the periodic read found, to tell how far the URCs can be trusted.
     */
    void populateRrcTelemetry(JsonObject& json) {
        std::scoped_lock lock(statusMutex);
        auto now = steady_clock::now();
        auto connectedTime = rrcConnectedTime;
        auto longestConnected = rrcLongestConnected;
        if (status.rrcConnected == true) {
            connectedTime += now - rrcConnectedSince;
            rrcConnectedSince = now;
            // Still going, so it may well turn out longer in the next report too
            longestConnected = std::max(longestConnected, now - rrcStretchStart);
        }
        auto elapsed = now - rrcLastReported;
        if (elapsed.count() > 0) {
            json["rrc-idle-ratio"] = 1.0 - (duration<double>(connectedTime) / duration<double>(elapsed));
            json["rrc-connections"] = rrcConnections;
            json["rrc-longest-connected"] = duration_cast<seconds>(longestConnected).count();
            json["rrc-missed-changes"] = rrcMissedChanges;
        }
        rrcLastReported = now;
        rrcConnectedTime = steady_clock::duration::zero();
        rrcConnections = 0;
        rrcLongestConnected = steady_clock::duration::zero();
        rrcMissedChanges = 0;
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

    // Runs on the UART's reader task, so it must not send commands to the modem
    void handleUrc(std::string_view line) {
        if (auto registration = parseCeregUrc(line)) {
            LOGTI(CELLULAR, "Network: %s", describe(*registration).c_str());
            networkChanged.overwrite(true);
            return;
        }
        if (line.starts_with("%XTIME:")) {
            if (auto time = parseXtime(line)) {
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
            updateRrcState(*connected, RrcSource::Urc);
            return;
        }
        if (auto edrx = parseCedrxp(line)) {
            LOGTI(CELLULAR, "eDRX changed by the network: %s", describe(*edrx).c_str());
            return;
        }
        if (line.starts_with("%MDMEV:")) {
            // Network searches, and the modem's own trouble: a reset loop, a rejected SIM
            LOGTI(CELLULAR, "Modem: %.*s", static_cast<int>(line.size()), line.data());
            return;
        }
        // Verbose only: a published log record is an uplink, and its acknowledgement brings the
        // modem's UART back up
        LOGTV(CELLULAR, "Unrecognized line from the modem: %.*s", static_cast<int>(line.size()), line.data());
    }

    /**
     * @brief Whether the console has the modem's UART: UD_UART0_CONSOLE builds put it back on
     * UART0, which is the modem's, on the same pins.
     */
    static bool sharesPinsWithConsole([[maybe_unused]] const CellularModemPins& pins) {
#if defined(CONFIG_ESP_CONSOLE_UART) && CONFIG_ESP_CONSOLE_UART_NUM == 0
        return true;
#else
        return false;
#endif
    }

    static constexpr milliseconds SEARCHING_POLL_INTERVAL = 30s;
    static constexpr milliseconds REGISTERED_POLL_INTERVAL = 5min;

    // How long to wait for NITZ after the network comes up before asking NTP. It comes with the
    // attach, within seconds of registering, and the network is only ready once the modem also
    // has an IP address
    static constexpr milliseconds NITZ_GRACE_PERIOD = 10s;
    // Keeps a due NTP request from spinning the loop
    static constexpr milliseconds MIN_POLL_INTERVAL = 1s;
    static constexpr milliseconds NTP_RETRY_INTERVAL = 5min;
    static constexpr milliseconds TIME_RESYNC_INTERVAL = 24h;

    // UART1 is the RS485 port's on MK14. UD_UART0_CONSOLE builds put the console on UART0, but
    // then the modem doesn't start (see sharesPinsWithConsole())
    static constexpr uart_port_t MODEM_UART = UART_NUM_0;

    const CellularModemPins pins;
    const std::vector<Rat> rats;
    // nullopt for eDRX off
    const std::optional<milliseconds> edrxCycle;
    // False for a boot that downloads a firmware update (see CellularModuleDriver::configure())
    const bool allowModuleSleep;
    StateSource& networkConnecting;
    StateSource& networkReady;
    const State& rtcInSync;
    const std::string ntpServer;
    const TimeHandler onNetworkTime;

    // A stall this long costs little at LTE-M rates, which fill the modem's TCP window in seconds
    static constexpr milliseconds BUSY_POLL_INTERVAL = 250ms;

    std::shared_ptr<CellularModuleDriver> module;
    AtSocketTransport mqttTransport { "MQTT" };
    // Polls while the image flows, as Serial Modem loses announcements while it's busy (see
    // AtSocketTransport). The UART stays on during an update anyway, so the reads cost nothing
    AtSocketTransport downloadTransport { "Download", BUSY_POLL_INTERVAL };

    // Cuts the wait between registration checks short
    CopyQueue<bool> networkChanged { "cellular-network", 1 };
    steady_clock::time_point networkReadySince;
    std::optional<steady_clock::time_point> lastNtpAttempt;
    std::optional<EdrxParameters> lastEdrx;
    // Set from the URC handler too
    std::atomic<steady_clock::time_point> lastTimeSync;

    mutable std::mutex statusMutex;
    CellularStatus status;
    // Guarded by statusMutex too
    // Since the start of the current telemetry interval, or of the connection if it's later
    steady_clock::time_point rrcConnectedSince;
    // Since the connection came up, across telemetry intervals
    steady_clock::time_point rrcStretchStart;
    steady_clock::duration rrcConnectedTime {};
    steady_clock::duration rrcLongestConnected {};
    // Counted from when the driver starts, like the ESP32's sleep time
    steady_clock::time_point rrcLastReported = steady_clock::now();
    uint32_t rrcConnections = 0;
    uint32_t rrcMissedChanges = 0;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
