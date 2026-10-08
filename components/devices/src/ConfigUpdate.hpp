#pragma once

#include <NvsStore.hpp>
#include <Queue.hpp>
#include <config/ConfigStateStore.hpp>
#include <drivers/LedDriver.hpp>
#include <functions/Function.hpp>
#include <mqtt/MqttRoot.hpp>

#include <cstdint>
#include <memory>
#include <string>

using namespace cornucopia::ugly_duckling::functions;
using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::config;
using namespace cornucopia::ugly_duckling::kernel::mqtt;

enum class ConfigUpdateResult : std::uint8_t {
    NoChanges,           // configurations absent or nothing differs from what's held
    DeviceChanged,       // staged into free slot, needs reboot to apply
    NetworkChanged,      // network config changed, needs reboot (MQTT parameters change)
    FunctionsApplied,    // hot-reloaded live, succeeded and committed
    FunctionsFailed,     // hot-reload failed, needs reboot to revert
};

/**
 * @brief Processes the `configurations` half of an `UPDATE` message: filters against held
 * fingerprints, stages into the free slot, then either reboots (device changed) or hot-reloads
 * (functions only). Returns what happened so the caller can decide the terminal action --
 * including whether a firmware download (handled separately) should subsume the reboot.
 */
ConfigUpdateResult applyConfigUpdate(
    JsonObjectConst configurations,
    const std::string& deviceConfirmedFingerprint,
    const std::string& networkConfirmedFingerprint,
    const std::shared_ptr<FunctionRegistry>& functionRegistry,
    const std::shared_ptr<ConfigStateStore>& configStateStore);

/**
 * @brief Subscribes to `update` (NoRetain, QoS 1) -- the combined config + firmware inbound path.
 *
 * Firmware updates are only started when the config state is fully confirmed (no pending
 * `requested`). If a firmware entry is present but a config request is still in flight, the
 * firmware entry is skipped and a `FailedPrecondition` rejection is reported via
 * `pendingFirmwareRejection` on the next SYNC. Config changes in the same UPDATE are processed
 * normally regardless.
 */
void registerUpdateHandler(
    const std::shared_ptr<MqttRoot>& mqttRoot,
    const std::string& deviceConfirmedFingerprint,
    const std::string& networkConfirmedFingerprint,
    const std::shared_ptr<FunctionRegistry>& functionRegistry,
    const std::shared_ptr<ConfigStateStore>& configStateStore,
    const std::shared_ptr<CopyQueue<bool>>& syncTriggerQueue,
    const std::shared_ptr<NvsStore>& nvs,
    const std::string& firmwareVersion,
    const std::shared_ptr<std::optional<RejectionCode>>& pendingFirmwareRejection);
