#include "BatteryManager.hpp"
#include "PowerManager.hpp"
#include "Queue.hpp"
#include "Task.hpp"
#include "Telemetry.hpp"
#include "Watchdog.hpp"
#include "drivers/BleDriver.hpp"
#include "drivers/WiFiDriver.hpp"
#include "mqtt/MqttDriver.hpp"
#include "mqtt/MqttRoot.hpp"
#include <TelemetryTask.hpp>

#include <bits/chrono.h>
#include <esp_heap_caps.h>

#include <chrono>
#include <cstdint>
#include <memory>

using namespace std::chrono;
using namespace cornucopia::ugly_duckling::kernel;

/**
 * Published at QoS 1. This used to be QoS 2 (issue #579) because esp-mqtt's outbox resends an
 * unacked PUBLISH verbatim (same packet id, DUP set) if the ack doesn't arrive within its
 * retransmit timeout while the connection stays up, and at QoS 1 the broker has no obligation
 * to dedup that resend before fanning it out to subscribers. That breaks every read-and-reset
 * value on this path: flow-meter `volume`, `pm.sleep-ratio` / `pm.sleep-count`, and the
 * `wifi.disconnects` / `mqtt.disconnects` counters.
 *
 * The server now dedups instead: telemetry rows are keyed on the device's own timestamp, and an
 * outbox resend is verbatim -- same payload, therefore same timestamp -- so the duplicate
 * collapses onto the row it already wrote. Flow volume is safe under this because the total is
 * a query-time SUM over rows, not a running accumulator.
 *
 * QoS 2 bought nothing end to end anyway: the broker downgrades to the subscriber's QoS on the
 * next hop, so the exactly-once guarantee stopped at the broker. QoS 1 halves the handshake
 * (PUBLISH/PUBACK vs. PUBLISH/PUBREC/PUBREL/PUBCOMP), which is one round trip less of modem-on
 * time per publish on a battery device (issue #634).
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
    const std::shared_ptr<CopyQueue<bool>>& telemetryPublishQueue) {
    Task::loop("telemetry", 8192, [publishInterval, watchdog, mqttRoot, batteryManager, powerManager, wifi, ble, telemetryCollector, telemetryPublishQueue](Task& task) {
        task.markWakeTime();

        if (batteryManager != nullptr) {
            ble->setBatteryLevel(static_cast<uint8_t>(batteryManager->getPercentage()));
        }

        mqttRoot->publish("telemetry", [batteryManager, powerManager, wifi, mqttRoot, telemetryCollector](JsonObject& telemetry) {
            telemetry["uptime"] = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
            telemetry["timestamp"] = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

            if (batteryManager != nullptr) {
                auto battery = telemetry["battery"].to<JsonObject>();
                battery["voltage"] = static_cast<double>(batteryManager->getVoltage()) / 1000.0;    // Convert to volts
                battery["percentage"] = batteryManager->getPercentage();
                auto current = batteryManager->getCurrent();
                if (current.has_value()) {
                    battery["current"] = *current;
                }
                auto timeToEmpty = batteryManager->getTimeToEmpty();
                if (timeToEmpty.has_value()) {
                    battery["time-to-empty"] = timeToEmpty->count();
                }
            }

            auto wifiData = telemetry["wifi"].to<JsonObject>();
            wifi->populateTelemetry(wifiData);

            auto mqttData = telemetry["mqtt"].to<JsonObject>();
            mqttRoot->mqtt->populateTelemetry(mqttData);

            auto memoryData = telemetry["memory"].to<JsonObject>();
            memoryData["free-heap"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            memoryData["min-heap"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

            auto powerManagementData = telemetry["pm"].to<JsonObject>();
            powerManager->populateTelemetry(powerManagementData);

            auto features = telemetry["features"].to<JsonArray>();
            telemetryCollector->collect(features); }, Retention::NoRetain, QoS::AtLeastOnce);

        // Signal that we are still alive
        watchdog->restart();

        // We always wait at least this much between telemetry updates
        const auto debounceInterval = 500ms;
        // Delay without updating last wake time
        Task::delay(task.ticksUntil(debounceInterval));

        // Allow other tasks to trigger telemetry updates
        auto timeout = task.ticksUntil(publishInterval - debounceInterval);
        telemetryPublishQueue->pollIn(timeout);
    });
}
