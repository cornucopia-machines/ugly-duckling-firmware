#pragma once

#include <BatteryManager.hpp>
#include <PowerManager.hpp>
#include <Queue.hpp>
#include <Telemetry.hpp>
#include <Watchdog.hpp>
#include <drivers/BleDriver.hpp>
#include <mqtt/MqttRoot.hpp>

#ifdef UD_CONNECTIVITY_WIFI
#include <drivers/WiFiDriver.hpp>
#endif

#include <chrono>
#include <memory>

using namespace std::chrono;
using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::mqtt;

/**
 * @brief Publishes `telemetry` (NoRetain, QoS 1) on the given interval.
 */
void initTelemetryPublishTask(
    milliseconds publishInterval,
    const std::shared_ptr<Watchdog>& watchdog,
    const std::shared_ptr<MqttRoot>& mqttRoot,
    const std::shared_ptr<BatteryManager>& batteryManager,
    const std::shared_ptr<PowerManager>& powerManager,
#ifdef UD_CONNECTIVITY_WIFI
    const std::shared_ptr<WiFiDriver>& wifi,
#endif
    const std::shared_ptr<BleDriver>& ble,
    const std::shared_ptr<TelemetryCollector>& telemetryCollector,
    const std::shared_ptr<CopyQueue<bool>>& telemetryPublishQueue);
