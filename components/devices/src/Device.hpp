#pragma once

#ifndef UD_PLATFORM
#error "UD_PLATFORM is not defined — set it via DeviceCommon.cmake"
#endif

// Helper macros to convert macro to string
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#include <esp_app_desc.h>

#include <chrono>
#include <concepts>
#include <memory>
#include <string>

static const std::string firmwareVersion(esp_app_get_description()->version);

#include <BootConfig.hpp>
#include <BootHelpers.hpp>
#include <BootMessage.hpp>
#include <ConfigUpdate.hpp>
#include <Connectivity.hpp>
#include <Console.hpp>
#include <DebugConsole.hpp>
#include <DeviceInit.hpp>
#include <FirmwareRollback.hpp>
#include <HardwareVersion.hpp>
#include <HeapTrace.hpp>
#include <HttpUpdate.hpp>
#include <Log.hpp>
#include <MacAddress.hpp>
#include <MqttCommands.hpp>
#include <NetworkConfig.hpp>
#include <ShutdownManager.hpp>
#include <Strings.hpp>
#include <SyncPublisher.hpp>
#include <TelemetryTask.hpp>
#include <config/ConfigBootPlan.hpp>
#include <config/NvsConfiguration.hpp>
#include <devices/DeviceDefinition.hpp>
#include <mqtt/MqttLog.hpp>

using namespace std::chrono;
using namespace cornucopia::ugly_duckling::devices;
using namespace cornucopia::ugly_duckling::functions;
using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::config;
using namespace cornucopia::ugly_duckling::peripherals;

template <std::derived_from<DeviceDefinition> TDeviceDefinition>
static void startDevice() {
    auto deviceDefinition = std::make_shared<TDeviceDefinition>();
    LOGD("Starting %s revision %d device",
        deviceDefinition->model.c_str(), deviceDefinition->revision);

    auto resetReason = esp_reset_reason();
    LOGD("Restarting after reset reason: %d", resetReason);

    auto i2c = std::make_shared<I2CManager>();
    auto battery = initBattery(deviceDefinition, i2c);

    initNvsFlash();

    // Install GPIO ISR service
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

#ifdef CONFIG_HEAP_TRACING
    ESP_ERROR_CHECK(heap_trace_init_standalone(trace_record, NUM_RECORDS));
#endif

    auto legacyConfigNvs = std::make_shared<NvsStore>("config");

    LOGTV(NVS, "NVS configurations:");
    legacyConfigNvs->list([](const std::string& key) {
        LOGTV(NVS, " - %s", key.c_str());
    });

    auto boot = loadDeviceBootConfig();

    // Network config: bootstrap-migrate from old NVS if needed, then load from the
    // confirmed config slot (docs/specs/device-readdressing.md, "Bootstrap migration").
    auto networkConfig = loadNetworkConfig(legacyConfigNvs, boot.configNvs, boot.networkManifestEntry);

    const std::string modelWithRevision = deviceDefinition->model + " (rev" + std::to_string(deviceDefinition->revision) + ")";

    auto watchdog = initWatchdog(boot.deviceConfig->watchdogTimeout.get());

    auto powerManager = std::make_shared<PowerManager>(boot.deviceConfig->sleepWhenIdle.get());

    auto logRecords = std::make_shared<Queue<LogRecord>>("logs",
#ifdef UD_DEBUG
        128
#else
        32
#endif
    );
    ConsoleProvider::init(logRecords, boot.deviceConfig->publishLogs.get());

    const auto& macAddress = getMacAddress();
    const auto& hardwareVersion = getHardwareVersion();

    LOGD("\n"
         "   _   _       _         ____             _    _ _\n"
         "  | | | | __ _| |_   _  |  _ \\ _   _  ___| | _| (_)_ __   __ _\n"
         "  | | | |/ _` | | | | | | | | | | | |/ __| |/ / | | '_ \\ / _` |\n"
         "  | |_| | (_| | | |_| | | |_| | |_| | (__|   <| | | | | | (_| |\n"
         "   \\___/ \\__, |_|\\__, | |____/ \\__,_|\\___|_|\\_\\_|_|_| |_|\\__, |\n"
         "         |___/   |___/                                    |___/ %s\n",
        firmwareVersion.c_str());
    LOGI("Initializing ugly duckling firmware version %s on %s with hostname '%s' and MAC address %s",
        firmwareVersion.c_str(),
        modelWithRevision.c_str(),
        networkConfig->getHostname(macAddress).c_str(),
        macAddress.c_str());

    auto statusLed = std::make_shared<LedDriver>("status", deviceDefinition->statusPin);
    auto states = std::make_shared<ModuleStates>();
    KernelStatusTask::init(statusLed, states);

    // Skip BLE while a firmware update is pending: the OTA's TLS handshake needs the RAM on
    // ESP32-C6, and the device reboots after the update attempt either way
    std::shared_ptr<BleDriver> ble;
    if (HttpUpdater::isUpdatePending(legacyConfigNvs)) {
        LOGI("Firmware update pending, not starting BLE");
        ble = std::make_shared<BleDriver>();
    } else {
        ble = initBle(boot.deviceConfig, networkConfig->getHostname(macAddress), "Ugly Duckling " + modelWithRevision, firmwareVersion, macAddress);
    }

    auto telemetryPublishQueue = std::make_shared<CopyQueue<bool>>("telemetry-publish", 1);
    auto telemetryPublisher = std::make_shared<TelemetryPublisher>(telemetryPublishQueue);

    // Init switch and button handling
    auto switches = std::make_shared<SwitchManager>();
    switches->registerSwitch({ //
        .name = "factory-reset",
        .pin = deviceDefinition->bootPin,
        .mode = SwitchMode::PullUp,
        .onDisengaged = [statusLed, deviceDefinition, telemetryPublisher](const SwitchEvent& event) {
            auto duration = event.timeSinceLastChange;
            if (duration >= 30s) {
                LOGI("Factory reset triggered after %lld ms", duration.count());
                performFactoryReset(statusLed, true);
            } else if (duration >= 5s) {
                LOGI("WiFi reset triggered after %lld ms", duration.count());
                performFactoryReset(statusLed, false);
            } else if (duration >= 1000ms) {
                LOGD("Publishing telemetry after %lld ms", duration.count());
                telemetryPublisher->requestTelemetryPublishing();
            } else {
                deviceDefinition->handleShortButtonPress(duration);
            }
        } });

    // Init battery management
    auto shutdownManager = std::make_shared<ShutdownManager>();
    std::shared_ptr<BatteryManager> batteryManager;
    if (battery != nullptr) {
        LOGD("Battery configured");
        batteryManager = std::make_shared<BatteryManager>(battery, shutdownManager);
    } else {
        LOGD("No battery configured");
    }

    auto connectivity = initConnectivity(states, networkConfig, ble);
    auto& wifi = connectivity.wifi;

#ifdef UD_DEBUG_CONSOLE
    new DebugConsole(batteryManager, wifi, ble);
#endif

    // Init MQTT connection
    // TODO(legacy-v1-topics): remove fallback and the macAddress parameter
    auto clientId = "ugly-duckling-" + (networkConfig->id.get().empty() ? macAddress : networkConfig->id.get());
    auto mqttRoot = initMqtt(states, clientId, networkConfig, states->mqttReady);
    MqttLog::init(boot.deviceConfig->publishLogs.get(), logRecords, mqttRoot);
    registerBasicCommands(mqttRoot);
    registerNvsCommands(mqttRoot);

    // SYNC trigger: a single-element overwrite queue coalesces every successful (re)connection
    // (docs/Configuration.md, "BOOT, SYNC, UPDATE") plus post-UPDATE requests into one pending
    // publish. The connected-listener callback must not publish inline (it runs on the MQTT
    // event-loop task, which publish() itself enqueues onto and waits for), so it only overwrites
    // the queue; initSyncTask below does the actual publish, from its own task.
    auto syncTriggerQueue = std::make_shared<CopyQueue<bool>>("sync-trigger", 1);
    mqttRoot->mqtt->onConnected([syncTriggerQueue]() {
        syncTriggerQueue->overwrite(true);
    });

    // Holds this boot's rejection code (if any) for the SYNC task to attach to the first SYNC it
    // publishes -- populated below, once the strict-boot outcome is known, strictly before
    // kernelReady is set (docs/Configuration.md, "Rejection reporting").
    auto pendingConfigRejection = std::make_shared<std::optional<RejectionCode>>();

    // Firmware rejection: same pattern as pendingConfigRejection, but for a failed firmware
    // download/install (docs/specs/done/firmware-update-via-sync-update.md, "Rejection reporting").
    // Populated here from performPendingHttpUpdateIfNecessary()'s return value, consumed once
    // by the first SYNC this boot publishes.
    auto pendingFirmwareRejection = std::make_shared<std::optional<RejectionCode>>();

    // Handle any pending HTTP update (reboots after the attempt, whether it succeeds or not)
    registerHttpUpdateCommand(mqttRoot, legacyConfigNvs);
    auto firmwareDownloadRejection = HttpUpdater::performPendingHttpUpdateIfNecessary(legacyConfigNvs, wifi, states->mqttReady, watchdog, firmwareVersion);

    // Detect whether the bootloader rolled back from a failed OTA partition. This and a failed
    // download cannot co-occur: a failed download never writes a new partition, so there's nothing
    // for the bootloader to roll back from. If both somehow fire (defense in depth), the rollback
    // takes priority — it's the more severe signal.
    auto rollback = detectAndClearRollback();

    // Thread whichever rejection source fired (at most one) to BOOT and SYNC
    if (rollback) {
        *pendingFirmwareRejection = rollback->rejectionCode;
    } else {
        *pendingFirmwareRejection = firmwareDownloadRejection;
    }

    mqttRoot->registerCommand("ping", [telemetryPublisher](const JsonObject&, JsonObject& response) {
        telemetryPublisher->requestTelemetryPublishing();
        response["pong"] = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    });

    // We want RTC to be in sync before we start setting up peripherals
    states->rtcInSync.awaitSet();

    auto runtime = initDeviceRuntime(i2c, mqttRoot, switches, telemetryPublisher,
        deviceDefinition, boot.deviceConfig, boot.configNvs, shutdownManager,
        boot.deviceManifestEntry);

    registerUpdateHandler(mqttRoot, boot.deviceManifestEntry.fingerprint, boot.networkManifestEntry.fingerprint, runtime.functionRegistry, boot.configStateStore, syncTriggerQueue, legacyConfigNvs, firmwareVersion, pendingFirmwareRejection);
    initSyncTask(mqttRoot, syncTriggerQueue, states, runtime.functionRegistry, runtime.deviceManifestEntry, boot.networkManifestEntry, pendingConfigRejection, pendingFirmwareRejection, firmwareVersion);

    // Booting a `requested` set is strict (docs/Configuration.md, "The confirmed/requested state
    // machine"): unlike `confirmed` (or the empty-slot default), which boots best-effort regardless
    // of errors, any peripheral/function apply error here is a detected failure -- revert to `confirmed` and
    // reboot immediately, never reaching the `boot`/`sync` publishes below for this failed attempt.
    // A hard crash before reaching this point is caught on the *next* boot instead, since the
    // pending -> attempted transition was already persisted above, before this attempt started.
    if (boot.bootPlan.strict) {
        bool success = (runtime.initState == InitState::Success);
        ConfigState outcome = recordStrictBootOutcome(boot.configState, boot.bootPlan.slotToLoad, success, RejectionCode::Internal);
        boot.configStateStore->save(outcome);
        if (!success) {
            LOGE("Requested configuration failed to apply (state=%d); reverting and rebooting",
                static_cast<int>(runtime.initState));
            delayedRestart();
            return;
        }
        boot.configState = outcome;
        LOGI("Requested configuration applied successfully; committed slot '%s' as confirmed",
            toString(boot.bootPlan.slotToLoad).c_str());
    }

    // A rejection recorded by this boot's revert (or an earlier one still unreported) is echoed on
    // this boot's BOOT message *and* on the first SYNC this boot publishes (docs/Configuration.md,
    // "Rejection reporting"): BOOT because it fires deterministically on every boot, unlike SYNC
    // which waits on kernelReady + a live MQTT connection, and SYNC too since it's what most
    // clients are already watching for reconciliation state. Cleared from config-state immediately
    // (not after confirmed delivery), matching this system's existing no-delivery-guarantees
    // posture; `pendingConfigRejection` carries the in-memory copy to the SYNC task, which consumes
    // it after its first publish so later SYNCs in this boot session don't repeat it.
    std::optional<RejectionCode> rejectionToReport = boot.configState.rejection;
    if (rejectionToReport) {
        ConfigState cleared = boot.configState;
        cleared.rejection.reset();
        boot.configStateStore->save(cleared);
    }
    *pendingConfigRejection = rejectionToReport;

    auto peripheralsInitJson = runtime.peripheralsInitDoc.template as<JsonArray>();
    auto functionsInitJson = runtime.functionsInitDoc.template as<JsonArray>();

    initTelemetryPublishTask(boot.deviceConfig->publishInterval.get(), watchdog, mqttRoot, batteryManager, powerManager, wifi, ble, runtime.telemetryCollector, telemetryPublishQueue);

    // Enable power saving once we are done initializing
    WiFiDriver::setPowerSaveMode(boot.deviceConfig->sleepWhenIdle.get());

    publishBootMessage(mqttRoot, resetReason, firmwareVersion, macAddress, networkConfig, runtime.initState, peripheralsInitJson, functionsInitJson,
        powerManager, deviceDefinition, hardwareVersion, rejectionToReport, firmwareDownloadRejection, rollback);

    states->kernelReady.set();

    // Confirm this firmware as valid, cancelling any pending rollback. Must happen after
    // kernelReady — until this call, the bootloader considers a freshly-flashed partition
    // PENDING_VERIFY and will automatically revert if the device resets.
    confirmFirmwareValid();

    LOGI("Device ready in %.2f s (kernel version %s on %s with hostname '%s' and IP '%s', SSID '%s', current time is %lld)",
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() / 1000.0,
        firmwareVersion.c_str(),
        modelWithRevision.c_str(),
        networkConfig->getHostname(macAddress).c_str(),
        wifi->getIp().value_or("<no-ip>").c_str(),
        wifi->getSsid().value_or("<no-ssid>").c_str(),
        duration_cast<seconds>(system_clock::now().time_since_epoch()).count());

#ifdef CONFIG_HEAP_TASK_TRACKING
    Task::loop("task-heaps", 4096, [](Task& task) {
        dumpPerTaskHeapInfo();
        Task::delay(ticks(5s));
    });
#endif

    vTaskDelete(nullptr);
}
