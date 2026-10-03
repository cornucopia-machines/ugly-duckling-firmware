#pragma once

#include <KernelStatus.hpp>
#include <NetworkConfig.hpp>
#include <drivers/BleDriver.hpp>
#include <drivers/RtcDriver.hpp>
#include <drivers/cellular/CellularModemPins.hpp>

#ifdef UD_CONNECTIVITY_WIFI
#include <drivers/WiFiDriver.hpp>
#endif
#ifdef UD_CONNECTIVITY_CELLULAR
#include <drivers/cellular/CellularDriver.hpp>
#endif

#include <esp_transport.h>

#include <memory>
#include <optional>

using namespace cornucopia::ugly_duckling::kernel;

/**
 * @brief Creates the network and RTC drivers for the links the build has (UD_CONNECTIVITY).
 *
 * WiFi wires up BLE <-> WiFi callbacks: scan requests, credential provisioning, connection
 * control, and status notifications. The RTC syncs over SNTP when there is WiFi, since that needs
 * lwIP; without it the time comes from the modem. Either way, BLE can set the time too, and
 * captures the RTC so it stays alive as long as BLE does.
 *
 * The NB-IoT modem (docs/specs/NB-IoT.md) throws on a board without a modem connector.
 */
struct ConnectivityDrivers {
#ifdef UD_CONNECTIVITY_WIFI
    std::shared_ptr<WiFiDriver> wifi;
#endif
#ifdef UD_CONNECTIVITY_CELLULAR
    std::shared_ptr<cellular::CellularDriver> cellular;
#endif
    std::shared_ptr<RtcDriver> rtc;
    // The transport MQTT connects over; nullptr for lwIP's own
    esp_transport_handle_t mqttTransport = nullptr;
};

ConnectivityDrivers initConnectivity(
    const std::shared_ptr<ModuleStates>& states,
    const std::shared_ptr<NetworkConfig>& networkConfig,
    const std::shared_ptr<BleDriver>& ble,
    const std::optional<cellular::CellularModemPins>& modemPins);
