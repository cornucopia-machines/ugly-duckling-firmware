#pragma once

#include <KernelStatus.hpp>
#include <NetworkConfig.hpp>
#include <NetworkLink.hpp>
#include <drivers/BleDriver.hpp>
#include <drivers/RtcDriver.hpp>
#include <drivers/WiFiDriver.hpp>
#include <drivers/cellular/CellularModemPins.hpp>

#ifdef UD_PLATFORM_CARROT
#include <drivers/cellular/CellularDriver.hpp>
#endif

#include <esp_transport.h>

#include <memory>
#include <optional>

using namespace cornucopia::ugly_duckling;
using namespace cornucopia::ugly_duckling::kernel;

/**
 * @brief Creates the network and RTC drivers for the link network-config asks for (`links`, see
 * chooseNetworkLink()). Only the driver for that link is created; the other one stays nullptr.
 *
 * WiFi wires up BLE <-> WiFi callbacks: scan requests, credential provisioning, connection
 * control, and status notifications. The RTC syncs over SNTP when there is WiFi, since that needs
 * lwIP; without it the time comes from the modem. Either way, BLE can set the time too, and
 * captures the RTC so it stays alive as long as BLE does.
 *
 * The NB-IoT modem (docs/specs/NB-IoT.md) is Carrot only, and needs a board with a modem
 * connector; asking for it anywhere else falls back to WiFi.
 */
struct ConnectivityDrivers {
    NetworkLink link = NetworkLink::WiFi;
    std::shared_ptr<WiFiDriver> wifi;
#ifdef UD_PLATFORM_CARROT
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
