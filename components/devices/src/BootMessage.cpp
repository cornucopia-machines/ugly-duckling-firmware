#include "FirmwareRollback.hpp"
#include "HardwareVersion.hpp"
#include "NetworkConfig.hpp"
#include "PowerManager.hpp"
#include "config/ConfigState.hpp"
#include "devices/DeviceDefinition.hpp"
#include "mqtt/MqttDriver.hpp"
#include "mqtt/MqttRoot.hpp"
#include <BootMessage.hpp>

#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include <bits/chrono.h>

#include <chrono>

namespace {
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
RTC_DATA_ATTR int bootCount = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)
}    // namespace

#include <CrashManager.hpp>

#include <memory>
#include <optional>
#include <string>

using namespace std::chrono;
using namespace cornucopia::ugly_duckling::kernel;

/**
 * BOOT carries diagnostics and per-peripheral/function error feedback, but no configuration
 * bodies (docs/Configuration.md, "BOOT, SYNC, UPDATE") -- fingerprints are reported separately
 * by SYNC (initSyncTask), gated on kernelReady. `rejection` is present only when a
 * requested-set revert (this boot or an earlier, unreported one) left one recorded. Published at
 * QoS 1, like telemetry, sync and responses (log is the one holdout, see MqttLog) -- BOOT's
 * payload carries no delta/counter state, so a duplicate delivery is harmless here (issue #634).
 *
 * Fire-and-forget: this used to block the boot task for up to 5s waiting for the broker's ack,
 * but the wait never affected delivery (the driver enqueues into esp-mqtt's outbox either way)
 * and the PublishStatus was discarded. All it did was delay `kernelReady` and, behind it,
 * `confirmFirmwareValid()` -- gating firmware confirmation on an unread MQTT ack, which left a
 * freshly-flashed partition in PENDING_VERIFY for 5s longer than necessary. BOOT still precedes
 * SYNC on the wire: both traverse the same eventQueue and client in FIFO order, and BOOT is
 * queued before `kernelReady` (which SYNC waits on) is set.
 */
void publishBootMessage(
    const std::shared_ptr<MqttRoot>& mqttRoot,
    esp_reset_reason_t resetReason,
    const std::string& firmwareVersion,
    const std::string& macAddress,
    const std::shared_ptr<NetworkConfig>& networkConfig,
    InitState initState,
    const JsonArray& peripheralsInitJson,
    const JsonArray& functionsInitJson,
    const std::shared_ptr<PowerManager>& powerManager,
    const std::shared_ptr<DeviceDefinition>& deviceDefinition,
    const std::optional<HardwareVersion>& hardwareVersion,
    const std::optional<RejectionCode>& rejectionToReport,
    const std::optional<RejectionCode>& firmwareDownloadRejection,
    const std::optional<RollbackDetection>& rollback) {
    mqttRoot->publish(
        "boot",
        [resetReason, firmwareVersion, macAddress, networkConfig, initState, peripheralsInitJson, functionsInitJson, powerManager, deviceDefinition, hardwareVersion, rejectionToReport, firmwareDownloadRejection, rollback](JsonObject& json) {
            json["model"] = deviceDefinition->model;
            json["revision"] = deviceDefinition->revision;
            json["platform"] = UD_PLATFORM;
            // TODO(legacy-v1-topics): stop sending instance once the server no longer reads it
            json["instance"] = networkConfig->instance.get();
            json["mac"] = macAddress;
            if (hardwareVersion.has_value()) {
                json["batch"] = hardwareVersion->batch;
                json["serial"] = hardwareVersion->serial;
            }
            json["version"] = firmwareVersion;
#ifdef UD_DEBUG
            json["debug"] = true;
#else
            json["debug"] = false;
#endif
            json["reset"] = resetReason;
            json["wakeup"] = esp_sleep_get_wakeup_causes();
            json["bootCount"] = bootCount++;
            json["time"] = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
            json["state"] = static_cast<int>(initState);
            json["peripherals"].to<JsonArray>().set(peripheralsInitJson);
            json["functions"].to<JsonArray>().set(functionsInitJson);
            json["sleepWhenIdle"] = powerManager->sleepWhenIdle;
            if (rejectionToReport) {
                json["rejection"] = static_cast<int>(*rejectionToReport);
            }
            // firmwareRejection: either a failed download or a rollback (at most one)
            if (rollback) {
                json["firmwareRejection"] = static_cast<int>(rollback->rejectionCode);
            } else if (firmwareDownloadRejection) {
                json["firmwareRejection"] = static_cast<int>(*firmwareDownloadRejection);
            }

            // When a rollback was detected, attribute any coredump to the failed partition's
            // version rather than the currently running (rolled-back-to) firmwareVersion
            std::optional<std::string> rolledBackFromVersion;
            if (rollback) {
                rolledBackFromVersion = rollback->failedVersion;
            }
            CrashManager::handleCrashReport(json, firmwareVersion, rolledBackFromVersion);
        },
        QoS::AtLeastOnce);
}
