#include "KernelStatus.hpp"
#include "Log.hpp"
#include "NetworkConfig.hpp"
#include "NetworkLink.hpp"
#include "drivers/BleDriver.hpp"
#include "drivers/RtcDriver.hpp"
#include "drivers/WiFiDriver.hpp"
#include "drivers/WifiApRecord.hpp"
#include "drivers/cellular/CellularModemPins.hpp"
#include <Connectivity.hpp>

#ifdef UD_PLATFORM_CARROT
#include "drivers/cellular/CellularDriver.hpp"
#endif

#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace cornucopia::ugly_duckling::kernel;

static void initWiFi(ConnectivityDrivers& drivers, const std::shared_ptr<ModuleStates>& states, const std::shared_ptr<NetworkConfig>& networkConfig, const std::shared_ptr<BleDriver>& ble) {
    auto wifi = std::make_shared<WiFiDriver>(
        states->networkConnecting,
        states->networkReady,
        states->configPortalRunning,
        networkConfig->getHostname());
    drivers.wifi = wifi;

    // Init real time clock, straight away: the SNTP setup has to beat the first DHCP lease
    drivers.rtc = std::make_shared<RtcDriver>(wifi->getNetworkReady(), networkConfig->ntp.get(), states->rtcInSync);

    ble->setOnWifiScanRequested([wifi, ble]() {
        wifi->startWifiScan([ble](const std::vector<WifiApRecord>& records) {
            ble->setScanResults(records);
        });
    });
    ble->setOnWifiCredentialsReceived([wifi](const std::string& ssid, const std::string& password) {
        wifi->setCredentials(ssid, password);
    });
    ble->setOnWifiControlReceived([wifi](const std::string& cmd) {
        if (cmd == "disconnect") {
            wifi->disconnect();
        } else if (cmd == "disable") {
            wifi->disable();
        }
    });
    wifi->setOnStatusChanged([ble](const std::string& status) {
        ble->setWifiStatus(status);
    });
}

#ifdef UD_PLATFORM_CARROT
static void initCellular(ConnectivityDrivers& drivers, const std::shared_ptr<ModuleStates>& states, const std::shared_ptr<NetworkConfig>& networkConfig, const cellular::CellularModemPins& modemPins, bool updatePending) {
    // No lwIP to run SNTP over: the time comes from the modem
    auto rtc = std::make_shared<RtcDriver>(states->rtcInSync);
    drivers.rtc = rtc;

    auto ntpServer = networkConfig->ntp.get()->host.get();
    auto cellular = std::make_shared<cellular::CellularDriver>(
        modemPins,
        networkConfig->cellular.get(),
        states->networkConnecting,
        states->networkReady,
        states->rtcInSync,
        ntpServer.empty() ? std::string(RtcDriver::DEFAULT_NTP_SERVER) : ntpServer,
        [rtc](time_t utcTime, const char* source) { rtc->setTime(utcTime, source); },
        // The download needs the modem awake, and its data pushed; the device reboots after it
        // either way
        !updatePending,
        updatePending);
    drivers.cellular = cellular;
    drivers.modemTransport = cellular->getTransport();
}
#endif

ConnectivityDrivers initConnectivity(
    const std::shared_ptr<ModuleStates>& states,
    const std::shared_ptr<NetworkConfig>& networkConfig,
    const std::shared_ptr<BleDriver>& ble,
    [[maybe_unused]] const std::optional<cellular::CellularModemPins>& modemPins,
    [[maybe_unused]] bool updatePending) {
#ifdef UD_PLATFORM_CARROT
    bool cellularAvailable = modemPins.has_value();
#else
    bool cellularAvailable = false;
#endif
    auto choice = chooseNetworkLink(networkConfig->links.get(), cellularAvailable);
    if (choice.error) {
        LOGE("Cannot use the links in network-config (%s), using WiFi instead", choice.error->c_str());
    }
    LOGI("Connecting over %s", toString(choice.link));

    ConnectivityDrivers drivers;
    drivers.link = choice.link;
    switch (choice.link) {
        case NetworkLink::WiFi:
            initWiFi(drivers, states, networkConfig, ble);
            break;
        case NetworkLink::Cellular:
#ifdef UD_PLATFORM_CARROT
            // chooseNetworkLink() only picks cellular when there are modem pins
            if (modemPins) {
                initCellular(drivers, states, networkConfig, *modemPins, updatePending);
            }
#endif
            break;
    }

    auto rtc = drivers.rtc;
    ble->setOnTimeReceived([rtc](time_t utcTime) { rtc->setTime(utcTime); });

    return drivers;
}
