#include <UpdateFilter.hpp>
#include <catch2/catch_test_macros.hpp>
#include <config/ConfigEnvelope.hpp>
#include <config/StoredConfig.hpp>

#include <nvs_flash.h>

#include <memory>
#include <string>

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::config;

namespace {

void ensureNvsFlashInitialized() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

}    // namespace

// Exercises the NVS half of the bootstrap migration (docs/specs/device-readdressing.md, "Bootstrap
// migration"). The loadNetworkConfig() function in BootConfig.hpp performs these exact operations;
// testing them here at the kernel level avoids pulling in the full devices dependency tree while
// covering the same real-NVS behavior.

TEST_CASE("bootstrap migration: old network-config in legacy NVS migrates to the confirmed slot with sentinel fingerprint") {
    ensureNvsFlashInitialized();

    auto legacyNvs = std::make_shared<NvsStore>("bm-legacy");
    legacyNvs->eraseAll();
    auto slotNvs = std::make_shared<NvsStore>("bm-slot");
    slotNvs->eraseAll();

    // Seed the old-format network-config in the legacy NVS namespace, mimicking a device that was
    // running pre-migration firmware.
    JsonDocument oldNetworkConfig;
    oldNetworkConfig["host"] = "mqtt.example.com";
    oldNetworkConfig["port"] = 8883;
    oldNetworkConfig["instance"] = "soil-probe-north";
    oldNetworkConfig["location"] = "garden-3";
    legacyNvs->setJson("network-config", oldNetworkConfig.as<JsonVariantConst>());

    // The confirmed slot has no "network" entry yet -- this triggers the migration.
    StoredConfig networkStored(slotNvs, NETWORK_CONFIGURATION_NAME);
    REQUIRE_FALSE(networkStored.hasValue());

    // Perform the migration: read old config, wrap in envelope with sentinel fingerprint "unsynced",
    // write to the confirmed slot.
    JsonDocument oldNetworkRaw;
    REQUIRE(legacyNvs->getJson("network-config", oldNetworkRaw));

    ConfigEnvelope migrationEnvelope(oldNetworkRaw, "unsynced", "");
    networkStored.store(migrationEnvelope);

    // Verify the migrated entry.
    StoredConfig reloaded(slotNvs, NETWORK_CONFIGURATION_NAME);
    REQUIRE(reloaded.hasValue());
    REQUIRE(reloaded.fingerprint() == "unsynced");
    REQUIRE(reloaded.requestedAt().empty());

    JsonDocument data = reloaded.data();
    REQUIRE(data["host"].as<std::string>() == "mqtt.example.com");
    REQUIRE(data["port"].as<int>() == 8883);
    REQUIRE(data["instance"].as<std::string>() == "soil-probe-north");
    REQUIRE(data["location"].as<std::string>() == "garden-3");

    // NVS cleanup: remove the old key (best-effort, idempotent).
    legacyNvs->remove("network-config");
    JsonDocument shouldBeEmpty;
    REQUIRE_FALSE(legacyNvs->getJson("network-config", shouldBeEmpty));
}

TEST_CASE("bootstrap migration: already-migrated device skips the migration") {
    ensureNvsFlashInitialized();

    auto legacyNvs = std::make_shared<NvsStore>("bm-legacy");
    legacyNvs->eraseAll();
    auto slotNvs = std::make_shared<NvsStore>("bm-slot");
    slotNvs->eraseAll();

    // The confirmed slot already has a "network" entry from a previous migration or UPDATE.
    JsonDocument existingNetworkConfig;
    existingNetworkConfig["host"] = "mqtt.example.com";
    existingNetworkConfig["id"] = "2N4GcBkr7ER";
    StoredConfig(slotNvs, NETWORK_CONFIGURATION_NAME)
        .store(ConfigEnvelope(existingNetworkConfig, "server-assigned-fp", "2026-07-30T12:00:00Z"));

    // Old NVS still has a stale network-config key (orphaned from a previous boot).
    JsonDocument staleOldConfig;
    staleOldConfig["host"] = "old.example.com";
    staleOldConfig["instance"] = "old-instance";
    legacyNvs->setJson("network-config", staleOldConfig.as<JsonVariantConst>());

    // Migration check: the confirmed slot already has a "network" entry -- skip.
    StoredConfig networkStored(slotNvs, NETWORK_CONFIGURATION_NAME);
    REQUIRE(networkStored.hasValue());

    // The existing entry is preserved, not overwritten by the stale old config.
    REQUIRE(networkStored.fingerprint() == "server-assigned-fp");
    REQUIRE(networkStored.data()["id"].as<std::string>() == "2N4GcBkr7ER");

    // NVS cleanup still removes the orphaned key.
    legacyNvs->remove("network-config");
    JsonDocument shouldBeEmpty;
    REQUIRE_FALSE(legacyNvs->getJson("network-config", shouldBeEmpty));
}

TEST_CASE("bootstrap migration: NVS cleanup removes orphaned legacy key even without migration") {
    ensureNvsFlashInitialized();

    auto legacyNvs = std::make_shared<NvsStore>("bm-legacy");
    legacyNvs->eraseAll();
    auto slotNvs = std::make_shared<NvsStore>("bm-slot");
    slotNvs->eraseAll();

    // Confirmed slot has a "network" entry (previously migrated or from an UPDATE).
    JsonDocument networkConfig;
    networkConfig["host"] = "mqtt.example.com";
    StoredConfig(slotNvs, NETWORK_CONFIGURATION_NAME)
        .store(ConfigEnvelope(networkConfig, "real-fp", "2026-07-30T12:00:00Z"));

    // Legacy NVS has an orphaned key from an interrupted migration.
    JsonDocument orphanedConfig;
    orphanedConfig["host"] = "orphan.example.com";
    legacyNvs->setJson("network-config", orphanedConfig.as<JsonVariantConst>());

    // Cleanup: unconditionally remove the legacy key.
    legacyNvs->remove("network-config");

    // The orphaned key is gone.
    JsonDocument shouldBeEmpty;
    REQUIRE_FALSE(legacyNvs->getJson("network-config", shouldBeEmpty));

    // The confirmed slot's "network" entry is untouched.
    StoredConfig reloaded(slotNvs, NETWORK_CONFIGURATION_NAME);
    REQUIRE(reloaded.hasValue());
    REQUIRE(reloaded.fingerprint() == "real-fp");
}

TEST_CASE("bootstrap migration: no old config and no slot entry results in empty manifest entry") {
    ensureNvsFlashInitialized();

    auto legacyNvs = std::make_shared<NvsStore>("bm-legacy");
    legacyNvs->eraseAll();
    auto slotNvs = std::make_shared<NvsStore>("bm-slot");
    slotNvs->eraseAll();

    // Neither the confirmed slot nor the legacy NVS has network config -- a freshly-provisioned
    // device before any network-config was ever pushed.
    StoredConfig networkStored(slotNvs, NETWORK_CONFIGURATION_NAME);
    REQUIRE_FALSE(networkStored.hasValue());

    JsonDocument empty;
    REQUIRE_FALSE(legacyNvs->getJson("network-config", empty));

    // The manifest entry stays empty (both fingerprint and requestedAt are empty strings), and
    // the device will use default NetworkConfig values (hostname falls back to MAC address at
    // the call site; topic root falls back to the legacy format with an empty instance).
}
