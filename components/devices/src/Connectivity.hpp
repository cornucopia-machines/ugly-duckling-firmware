#pragma once

#include <KernelStatus.hpp>
#include <NetworkConfig.hpp>
#include <drivers/BleDriver.hpp>
#include <drivers/RtcDriver.hpp>
#include <drivers/WiFiDriver.hpp>
#include <drivers/cellular/CellularModemPins.hpp>

#ifdef UD_CONNECTIVITY_CELLULAR
#include <drivers/cellular/CellularDriver.hpp>
#endif

#include <memory>
#include <optional>

using namespace cornucopia::ugly_duckling::kernel;

/**
 * @brief Creates WiFi + RTC drivers and wires up BLE ↔ WiFi callbacks (time sync, scan
 * requests, credential provisioning, connection control, and status notifications).
 *
 * Returns WiFi and RTC drivers; RTC is also captured by BLE closures so it stays alive as
 * long as BLE does.
 *
 * With UD_CONNECTIVITY=CELLULAR, also starts the NB-IoT modem on boards that have one. For now
 * it runs next to WiFi, which still carries all traffic (docs/specs/NB-IoT.md, stage 2).
 */
struct ConnectivityDrivers {
    std::shared_ptr<WiFiDriver> wifi;
    std::shared_ptr<RtcDriver> rtc;
#ifdef UD_CONNECTIVITY_CELLULAR
    std::shared_ptr<cellular::CellularDriver> cellular;
#endif
};

ConnectivityDrivers initConnectivity(
    const std::shared_ptr<ModuleStates>& states,
    const std::shared_ptr<NetworkConfig>& networkConfig,
    const std::shared_ptr<BleDriver>& ble,
    const std::optional<cellular::CellularModemPins>& modemPins);
