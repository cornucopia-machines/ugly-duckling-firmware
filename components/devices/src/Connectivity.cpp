#include "KernelStatus.hpp"
#include "NetworkConfig.hpp"
#include "drivers/BleDriver.hpp"
#include "drivers/RtcDriver.hpp"
#include "drivers/cellular/CellularModemPins.hpp"
#include <Connectivity.hpp>

#ifdef UD_CONNECTIVITY_WIFI
#include "drivers/WiFiDriver.hpp"
#include "drivers/WifiApRecord.hpp"

#include <vector>
#endif
#ifdef UD_CONNECTIVITY_CELLULAR
#include "drivers/cellular/CellularDriver.hpp"

#include <stdexcept>
#endif

#include <ctime>
#include <memory>
#include <optional>
#include <string>

using namespace cornucopia::ugly_duckling::kernel;

ConnectivityDrivers initConnectivity(
    const std::shared_ptr<ModuleStates>& states,
    const std::shared_ptr<NetworkConfig>& networkConfig,
    const std::shared_ptr<BleDriver>& ble,
    [[maybe_unused]] const std::optional<cellular::CellularModemPins>& modemPins) {
    ConnectivityDrivers drivers;

#ifdef UD_CONNECTIVITY_WIFI
    auto wifi = std::make_shared<WiFiDriver>(
        states->networkConnecting,
        states->networkReady,
        states->configPortalRunning,
        networkConfig->getHostname());
    drivers.wifi = wifi;

    // Init real time clock, straight away: the SNTP setup has to beat the first DHCP lease
    auto rtc = std::make_shared<RtcDriver>(wifi->getNetworkReady(), networkConfig->ntp.get(), states->rtcInSync);

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
#else
    // No lwIP to run SNTP over: the time comes from the modem
    auto rtc = std::make_shared<RtcDriver>(states->rtcInSync);
#endif
    ble->setOnTimeReceived([rtc](time_t utcTime) { rtc->setTime(utcTime); });
    drivers.rtc = rtc;

#ifdef UD_CONNECTIVITY_CELLULAR
    if (!modemPins) {
        // A cellular build on such a board could never connect, so don't pretend to start
        throw std::runtime_error("Built for cellular connectivity, but this board has no modem connector");
    }

    auto ntpServer = networkConfig->ntp.get()->host.get();
    auto cellular = std::make_shared<cellular::CellularDriver>(
        *modemPins,
        states->networkConnecting,
        states->networkReady,
        states->rtcInSync,
        ntpServer.empty() ? std::string(RtcDriver::DEFAULT_NTP_SERVER) : ntpServer,
        [rtc](time_t utcTime, const char* source) { rtc->setTime(utcTime, source); });
    drivers.cellular = cellular;
    drivers.mqttTransport = cellular->getTransport();
#endif

    return drivers;
}
