#pragma once
#include <KernelStatus.hpp>
#include <Queue.hpp>
#include <config/ConfigState.hpp>
#include <functions/Function.hpp>
#include <mqtt/MqttRoot.hpp>

#include <memory>
#include <optional>
#include <string>

using namespace cornucopia::ugly_duckling::functions;
using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::config;

/**
 * @brief Publishes `sync` (NoRetain, QoS 1): the manifest of fingerprints/requestedAt the device
 * currently holds, plus the firmware identity (`platform`, `version`) the server needs for
 * firmware reconciliation (docs/specs/done/firmware-update-via-sync-update.md).
 */
void publishSync(
    const std::shared_ptr<MqttRoot>& mqttRoot,
    const std::shared_ptr<FunctionRegistry>& functionRegistry,
    const ConfigManifestEntry& deviceManifestEntry,
    const ConfigManifestEntry& networkManifestEntry,
    const std::shared_ptr<std::optional<RejectionCode>>& pendingConfigRejection,
    const std::shared_ptr<std::optional<RejectionCode>>& pendingFirmwareRejection,
    const std::string& firmwareVersion);

/**
 * @brief Dedicated task that publishes SYNC whenever triggered via syncTriggerQueue.
 */
void initSyncTask(
    const std::shared_ptr<MqttRoot>& mqttRoot,
    const std::shared_ptr<CopyQueue<bool>>& syncTriggerQueue,
    const std::shared_ptr<ModuleStates>& states,
    const std::shared_ptr<FunctionRegistry>& functionRegistry,
    const ConfigManifestEntry& deviceManifestEntry,
    const ConfigManifestEntry& networkManifestEntry,
    const std::shared_ptr<std::optional<RejectionCode>>& pendingConfigRejection,
    const std::shared_ptr<std::optional<RejectionCode>>& pendingFirmwareRejection,
    const std::string& firmwareVersion);
