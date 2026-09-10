#pragma once

#include <BatteryManager.hpp>
#include <PowerManager.hpp>
#include <Queue.hpp>
#include <Telemetry.hpp>
#include <Watchdog.hpp>
#include <drivers/BleDriver.hpp>
#include <drivers/WiFiDriver.hpp>
#include <mqtt/MqttRoot.hpp>

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
    const std::shared_ptr<WiFiDriver>& wifi,
    const std::shared_ptr<BleDriver>& ble,
    const std::shared_ptr<TelemetryCollector>& telemetryCollector,
    const std::shared_ptr<CopyQueue<bool>>& telemetryPublishQueue);
