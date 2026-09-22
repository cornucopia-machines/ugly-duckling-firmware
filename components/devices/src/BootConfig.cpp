#include "NvsStore.hpp"
#include "config/ConfigBootPlan.hpp"
#include "config/ConfigManifestEntry.hpp"
#include "config/ConfigState.hpp"
#include "config/ConfigStateStore.hpp"
#include "devices/DeviceConfiguration.hpp"
#include <BootConfig.hpp>
#include <Log.hpp>
#include <NetworkConfig.hpp>
#include <UpdateFilter.hpp>
#include <config/ConfigEnvelope.hpp>
#include <config/StoredConfig.hpp>

#include <memory>
#include <stdexcept>

using namespace cornucopia::ugly_duckling::devices;
using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::config;

/**
 * Two-slot confirmed/requested atomicity (docs/Configuration.md, "The confirmed/requested state
 * machine"). A device-changed UPDATE (registerUpdateHandler) is what populates `requested`,
 * staging a self-contained set into the free slot; the strict/revert machinery here is what
 * boots it. `configStateStore` is a shared_ptr so registerUpdateHandler's subscription closure
 * can load/save it live, long after startDevice()'s locals would otherwise have gone out of
 * scope (startDevice() never returns).
 */
DeviceBootConfig loadDeviceBootConfig() {
    auto configStateNvs = std::make_shared<NvsStore>("config-state");
    auto configStateStore = std::make_shared<ConfigStateStore>(configStateNvs);
    ConfigState configState = configStateStore->load();

    // Guarantee: a confirmed slot always exists. A fresh device (or one migrating from
    // pre-Phase-3 firmware) starts with an empty slot A -- defaults, no functions -- and
    // reconciles from scratch via an empty SYNC prompting the server to re-push everything (see
    // docs/specs/done/config-reconciliation.md, "Migration"). The slot itself is empty (no device
    // or function entries in NVS), but having the pointer means downstream code never has to
    // branch on "no slot at all" vs "empty slot".
    if (!configState.confirmed) {
        configState.confirmed = ConfigSlot::A;
        configStateStore->save(configState);
        LOGI("No confirmed config slot found; initialized empty slot 'a'");
    }

    BootPlan bootPlan = decideBootPlan(configState);

    LOGD("Booting from slot '%s', strict: %s, crash recovery checkpoint to persist: %s",
        toString(bootPlan.slotToLoad).c_str(),
        bootPlan.strict ? "true" : "false",
        bootPlan.crashRecoveryCheckpoint ? "true" : "false");

    if (bootPlan.crashRecoveryCheckpoint) {
        configStateStore->save(*bootPlan.crashRecoveryCheckpoint);
        configState = *bootPlan.crashRecoveryCheckpoint;
    }

    // Device configuration is stored as a verbatim envelope like any other reconciled
    // configuration (docs/Configuration.md, "Storage: envelopes and slots"), so its fingerprint is
    // available to the `update` handler without recomputing anything.
    auto configNvs = std::make_shared<NvsStore>("config-" + toString(bootPlan.slotToLoad));
    auto deviceConfig = std::make_shared<DeviceConfiguration>();
    ConfigManifestEntry deviceManifestEntry;
    StoredConfig deviceStoredConfig(configNvs, DEVICE_CONFIGURATION_NAME);
    if (deviceStoredConfig.hasValue()) {
        JsonDocument deviceConfigRaw = deviceStoredConfig.data();
        deviceConfig->load(deviceConfigRaw.as<JsonObject>());
        deviceManifestEntry = { .fingerprint = deviceStoredConfig.fingerprint(), .requestedAt = deviceStoredConfig.requestedAt() };
    }

    return {
        .configStateStore = configStateStore,
        .configState = configState,
        .bootPlan = bootPlan,
        .configNvs = configNvs,
        .deviceConfig = deviceConfig,
        .deviceManifestEntry = deviceManifestEntry,
        .networkManifestEntry = {},
    };
}

/**
 * Bootstrap migration + network config loading (docs/specs/device-readdressing.md, "Bootstrap
 * migration" and "NVS cleanup").
 *
 * The migration writes the network entry directly into slotNvs, which is bootPlan.slotToLoad.
 * This is safe because slotToLoad is always the confirmed slot when the migration runs:
 *
 *  - The spec's precondition (section "Precondition: clean config state before firmware upgrade")
 *    guarantees no pending `requested` on first boot with this firmware. Two independent guards
 *    enforce this: (1) the server avoids pushing firmware to devices with pending config, and
 *    (2) decideFirmwareUpdate() skips the firmware entry when configState.requested is present.
 *  - With no pending `requested`, decideBootPlan() returns slotToLoad = confirmed.
 *  - The migration only runs once (when the confirmed slot has no `network` entry), so it
 *    cannot trigger on a later boot where a `requested` might be pending.
 */
std::shared_ptr<NetworkConfig> loadNetworkConfig(
    const std::shared_ptr<NvsStore>& legacyConfigNvs,
    const std::shared_ptr<NvsStore>& slotNvs,
    ConfigManifestEntry& manifestEntry) {

    auto networkConfig = std::make_shared<NetworkConfig>();
    manifestEntry = {};

    if (!slotNvs) {
        throw std::runtime_error("No confirmed config slot -- network config requires a slot to load from");
    }

    StoredConfig networkStored(slotNvs, NETWORK_CONFIGURATION_NAME);

    // TODO(legacy-v1-topics): remove bootstrap migration once all devices have been migrated
    // Bootstrap migration: if the confirmed slot has no network entry yet, this is the first boot
    // after a firmware upgrade. Move the old NVS network-config into the confirmed slot with the
    // sentinel fingerprint "unsynced" so the SYNC/UPDATE protocol picks it up.
    if (!networkStored.hasValue()) {
        JsonDocument oldNetworkRaw;
        if (legacyConfigNvs->getJson("network-config", oldNetworkRaw)) {
            ConfigEnvelope migrationEnvelope(oldNetworkRaw, "unsynced", "");
            networkStored.store(migrationEnvelope);
            LOGI("Migrated network-config from NVS to confirmed config slot with sentinel fingerprint 'unsynced'");
        }
    }

    // Load network config from the slot (which now includes the migrated data, if applicable)
    if (networkStored.hasValue()) {
        JsonDocument networkRaw = networkStored.data();
        networkConfig->load(networkRaw.as<JsonObject>());
        manifestEntry = { .fingerprint = networkStored.fingerprint(), .requestedAt = networkStored.requestedAt() };
    }

    // TODO(legacy-v1-topics): remove NVS cleanup and the legacyConfigNvs parameter
    // NVS cleanup: remove any lingering network-config from the old namespace (best-effort,
    // idempotent -- handles orphaned keys from interrupted migrations)
    if (legacyConfigNvs->contains("network-config")) {
        LOGI("Removing orphaned legacy network-config from NVS");
        legacyConfigNvs->remove("network-config");
    }

    return networkConfig;
}
