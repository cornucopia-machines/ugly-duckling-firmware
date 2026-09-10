#include "KernelStatus.hpp"
#include "Queue.hpp"
#include "Task.hpp"
#include "config/ConfigManifestEntry.hpp"
#include "config/ConfigState.hpp"
#include "functions/Function.hpp"
#include "functions/FunctionConfigTracker.hpp"
#include "mqtt/MqttDriver.hpp"
#include "mqtt/MqttRoot.hpp"
#include <SyncPublisher.hpp>
#include <UpdateFilter.hpp>

#include <memory>
#include <optional>
#include <string>

using namespace cornucopia::ugly_duckling::functions;
using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::config;

/**
 * All read straight from in-memory state, never re-derived from NVS (docs/Configuration.md,
 * "BOOT, SYNC, UPDATE"). `deviceManifestEntry` is the device's own fingerprint/requestedAt:
 * unlike a function's, it can be captured once at boot and passed through unchanged for the
 * life of the process, since a device configuration change always reboots rather than
 * hot-reloading (see registerUpdateHandler).
 *
 * `pendingConfigRejection` carries this boot's config rejection code (if any) so it can ride on the
 * first SYNC published after boot, alongside BOOT (docs/Configuration.md, "Rejection reporting")
 * -- it is consumed (reset to nullopt) right here, so a later SYNC in the same boot session doesn't
 * repeat it. `pendingFirmwareRejection` works the same way for a failed firmware download/install
 * (docs/specs/done/firmware-update-via-sync-update.md, "Rejection reporting").
 */
void publishSync(
    const std::shared_ptr<MqttRoot>& mqttRoot,
    const std::shared_ptr<FunctionRegistry>& functionRegistry,
    const ConfigManifestEntry& deviceManifestEntry,
    const ConfigManifestEntry& networkManifestEntry,
    const std::shared_ptr<std::optional<RejectionCode>>& pendingConfigRejection,
    const std::shared_ptr<std::optional<RejectionCode>>& pendingFirmwareRejection,
    const std::string& firmwareVersion) {
    std::optional<RejectionCode> configRejection = *pendingConfigRejection;
    *pendingConfigRejection = std::nullopt;
    std::optional<RejectionCode> firmwareRejection = *pendingFirmwareRejection;
    *pendingFirmwareRejection = std::nullopt;
    mqttRoot->publish(
        "sync",
        [functionRegistry, deviceManifestEntry, networkManifestEntry, configRejection, firmwareRejection, firmwareVersion](JsonObject& json) {
            auto configurations = json["configurations"].to<JsonObject>();
            auto manifest = functionRegistry->manifest();
            manifest[DEVICE_CONFIGURATION_NAME] = deviceManifestEntry;
            manifest[NETWORK_CONFIGURATION_NAME] = networkManifestEntry;
            writeSyncManifest(configurations, manifest);
            if (configRejection) {
                json["rejection"] = static_cast<int>(*configRejection);
            }
            auto firmware = json["firmware"].to<JsonObject>();
            firmware["platform"] = UD_PLATFORM;
            firmware["version"] = firmwareVersion;
            if (firmwareRejection) {
                firmware["rejection"] = static_cast<int>(*firmwareRejection);
            }
        },
        Retention::NoRetain, QoS::AtLeastOnce);
}

/**
 * A single-element overwrite queue, so a flurry of triggers (reconnects, or a post-UPDATE request)
 * coalesces into one pending SYNC. Awaits kernelReady before publishing so SYNC only ever reports
 * a fully-booted configuration set: if MQTT connects before boot finishes, the publish simply
 * waits. There is no boot-time SYNC -- this task is the only publisher.
 *
 * Any further triggers that arrive while we are waiting on kernelReady (e.g. a flaky reconnect
 * during a slow boot) are drained right before publishing rather than left for the next loop
 * iteration: since publishSync() always reads FunctionRegistry's current state rather than a
 * snapshot from trigger time, the about-to-happen publish already answers them, so leaving them
 * queued would only cause an immediate, redundant re-publish straight after this one.
 *
 * `pendingConfigRejection` and `pendingFirmwareRejection` are populated by the caller
 * (`startDevice()`) strictly before `kernelReady` is set, so `awaitSet()` below establishes
 * happens-after visibility of them regardless of how early a trigger arrives.
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
    const std::string& firmwareVersion) {
    Task::loop("sync", 4096, [mqttRoot, syncTriggerQueue, states, functionRegistry, deviceManifestEntry, networkManifestEntry, pendingConfigRejection, pendingFirmwareRejection, firmwareVersion](Task&) {
        syncTriggerQueue->take();
        states->kernelReady.awaitSet();
        syncTriggerQueue->clear();
        publishSync(mqttRoot, functionRegistry, deviceManifestEntry, networkManifestEntry, pendingConfigRejection, pendingFirmwareRejection, firmwareVersion);
    });
}
