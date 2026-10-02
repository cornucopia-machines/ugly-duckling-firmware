#include "KernelStatus.hpp"
#include "NetworkConfig.hpp"
#include "drivers/BleDriver.hpp"
#include "drivers/RtcDriver.hpp"
#include "drivers/WiFiDriver.hpp"
#include "drivers/WifiApRecord.hpp"
#include "drivers/cellular/CellularModemPins.hpp"
#include <Connectivity.hpp>

#ifdef UD_CONNECTIVITY_CELLULAR
#include "drivers/cellular/CellularDriver.hpp"
#include <Log.hpp>
#endif

#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace cornucopia::ugly_duckling::kernel;

ConnectivityDrivers initConnectivity(
    const std::shared_ptr<ModuleStates>& states,
    const std::shared_ptr<NetworkConfig>& networkConfig,
    const std::shared_ptr<BleDriver>& ble,
    [[maybe_unused]] const std::optional<cellular::CellularModemPins>& modemPins) {

    auto wifi = std::make_shared<WiFiDriver>(
        states->networkConnecting,
        states->networkReady,
        states->configPortalRunning,
        networkConfig->getHostname());

    // Init real time clock
    auto rtc = std::make_shared<RtcDriver>(wifi->getNetworkReady(), networkConfig->ntp.get(), states->rtcInSync);
    ble->setOnTimeReceived([rtc](time_t utcTime) { rtc->setTime(utcTime); });
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

#ifdef UD_CONNECTIVITY_CELLULAR
    std::shared_ptr<cellular::CellularDriver> cellular;
    if (modemPins) {
        cellular = std::make_shared<cellular::CellularDriver>(*modemPins);
    } else {
        LOGW("Built for cellular connectivity, but this board has no modem connector");
    }
    return { .wifi = wifi, .rtc = rtc, .cellular = cellular };
#else
    return { .wifi = wifi, .rtc = rtc };
#endif
}
