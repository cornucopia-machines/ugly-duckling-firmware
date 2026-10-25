#include <catch2/catch_test_macros.hpp>
#include <functions/FunctionConfigTracker.hpp>

#include <string>

using namespace cornucopia::ugly_duckling::functions;

TEST_CASE("manifest is empty before anything is recorded") {
    FunctionConfigTracker tracker;
    REQUIRE(tracker.manifest().empty());
}

TEST_CASE("manifest reports fingerprints and requestedAt recorded at creation") {
    FunctionConfigTracker tracker;
    tracker.record("valve1", [](JsonObjectConst) -> Result<> { return {}; }, "fp-1", "2026-07-30T12:00:00Z");
    tracker.record("valve2", [](JsonObjectConst) -> Result<> { return {}; }, "fp-2", "2026-07-30T13:00:00Z");

    auto manifest = tracker.manifest();
    REQUIRE(manifest.size() == 2);
    REQUIRE(manifest.at("valve1").fingerprint == "fp-1");
    REQUIRE(manifest.at("valve1").requestedAt == "2026-07-30T12:00:00Z");
    REQUIRE(manifest.at("valve2").fingerprint == "fp-2");
    REQUIRE(manifest.at("valve2").requestedAt == "2026-07-30T13:00:00Z");
}

TEST_CASE("apply invokes the configureFn with the given data") {
    FunctionConfigTracker tracker;
    JsonObjectConst received;
    tracker.record("valve1", [&](JsonObjectConst data) -> Result<> { received = data; return {}; }, "fp-1", "2026-07-30T12:00:00Z");

    JsonDocument body;
    body["openDuration"] = 30;
    REQUIRE(tracker.apply("valve1", body.as<JsonObjectConst>(), "fp-2", "2026-07-30T13:00:00Z"));

    REQUIRE(received["openDuration"].as<int>() == 30);
}

TEST_CASE("apply records the new fingerprint and requestedAt only after a successful configureFn") {
    FunctionConfigTracker tracker;
    tracker.record("valve1", [](JsonObjectConst) -> Result<> { return {}; }, "fp-1", "2026-07-30T12:00:00Z");

    JsonDocument body;
    REQUIRE(tracker.apply("valve1", body.as<JsonObjectConst>(), "fp-2", "2026-07-30T13:00:00Z"));

    auto entry = tracker.manifest().at("valve1");
    REQUIRE(entry.fingerprint == "fp-2");
    REQUIRE(entry.requestedAt == "2026-07-30T13:00:00Z");
}

TEST_CASE("apply does not update the fingerprint or requestedAt when configureFn fails") {
    FunctionConfigTracker tracker;
    tracker.record("valve1", [](JsonObjectConst) -> Result<> { return failure("bad config"); }, "fp-1", "2026-07-30T12:00:00Z");

    JsonDocument body;
    auto result = tracker.apply("valve1", body.as<JsonObjectConst>(), "fp-2", "2026-07-30T13:00:00Z");
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message == "bad config");

    auto entry = tracker.manifest().at("valve1");
    REQUIRE(entry.fingerprint == "fp-1");
    REQUIRE(entry.requestedAt == "2026-07-30T12:00:00Z");
}

TEST_CASE("apply fails for an unknown function name") {
    FunctionConfigTracker tracker;

    JsonDocument body;
    auto result = tracker.apply("unknown", body.as<JsonObjectConst>(), "fp-1", "2026-07-30T12:00:00Z");
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message == "Cannot reconfigure unknown function 'unknown'");
}

TEST_CASE("apply fails for a function recorded without a configureFn") {
    FunctionConfigTracker tracker;
    tracker.record("valve1", nullptr, "", "");

    JsonDocument body;
    auto result = tracker.apply("valve1", body.as<JsonObjectConst>(), "fp-1", "2026-07-30T12:00:00Z");
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message == "Function 'valve1' does not support configuration");
}

TEST_CASE("writeSyncManifest writes nothing for an empty manifest") {
    JsonDocument doc;
    auto configurations = doc.to<JsonObject>();
    writeSyncManifest(configurations, {});

    REQUIRE(configurations.size() == 0);
}

TEST_CASE("writeSyncManifest writes fingerprint and requestedAt per function, keyed by name") {
    JsonDocument doc;
    auto configurations = doc.to<JsonObject>();
    writeSyncManifest(configurations, {
                                          { "valve1", { .fingerprint = "fp-1", .requestedAt = "2026-07-30T12:00:00Z" } },
                                          { "door1", { .fingerprint = "fp-2", .requestedAt = "2026-07-30T13:00:00Z" } },
                                      });

    REQUIRE(configurations.size() == 2);
    REQUIRE(configurations["valve1"]["fingerprint"].as<std::string>() == "fp-1");
    REQUIRE(configurations["valve1"]["requestedAt"].as<std::string>() == "2026-07-30T12:00:00Z");
    REQUIRE(configurations["door1"]["fingerprint"].as<std::string>() == "fp-2");
    REQUIRE(configurations["door1"]["requestedAt"].as<std::string>() == "2026-07-30T13:00:00Z");
}

TEST_CASE("writeSyncManifest echoes an empty fingerprint verbatim (unconfirmed / legacy-adopted config)") {
    JsonDocument doc;
    auto configurations = doc.to<JsonObject>();
    writeSyncManifest(configurations, { { "valve1", { .fingerprint = "", .requestedAt = "" } } });

    REQUIRE(configurations["valve1"]["fingerprint"].as<std::string>().empty());
    REQUIRE(configurations["valve1"]["requestedAt"].as<std::string>().empty());
}
