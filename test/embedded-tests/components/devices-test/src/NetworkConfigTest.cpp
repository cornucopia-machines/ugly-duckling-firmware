#include <NetworkConfig.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

// Exercises NetworkConfig parsing and derived values (topic root, hostname) with both old-format
// and new-format JSON shapes (docs/specs/device-readdressing.md, "New network-config shape").

TEST_CASE("NetworkConfig: new shape with id parses correctly") {
    NetworkConfig config;
    REQUIRE(config.loadFromString(R"({
        "id": "2N4GcBkr7ER",
        "host": "mqtt.example.com",
        "port": 8883,
        "ntp": { "host": "pool.ntp.org" }
    })"));

    REQUIRE(config.id.get() == "2N4GcBkr7ER");
    REQUIRE(config.host.get() == "mqtt.example.com");
    REQUIRE(config.port.get() == 8883);
    REQUIRE(config.ntp.get()->host.get() == "pool.ntp.org");
    // instance and location are absent in new-shape configs
    REQUIRE(config.instance.get().empty());
    REQUIRE(config.location.get().empty());
}

TEST_CASE("NetworkConfig: new shape uses id-based topic root") {
    NetworkConfig config;
    REQUIRE(config.loadFromString(R"({"id": "2N4GcBkr7ER"})"));

    REQUIRE(config.getTopicRoot() == "d/2N4GcBkr7ER");
}

TEST_CASE("NetworkConfig: new shape uses id as hostname") {
    NetworkConfig config;
    REQUIRE(config.loadFromString(R"({"id": "2N4GcBkr7ER"})"));

    REQUIRE(config.getHostname() == "2N4GcBkr7ER");
}

TEST_CASE("NetworkConfig: old shape with instance and location parses correctly") {
    NetworkConfig config;
    REQUIRE(config.loadFromString(R"({
        "host": "mqtt.example.com",
        "port": 8883,
        "instance": "soil-probe-north",
        "location": "garden-3",
        "ntp": { "host": "pool.ntp.org" }
    })"));

    REQUIRE(config.instance.get() == "soil-probe-north");
    REQUIRE(config.location.get() == "garden-3");
    REQUIRE(config.host.get() == "mqtt.example.com");
    REQUIRE(config.port.get() == 8883);
    // id is absent in old-shape configs
    REQUIRE(config.id.get().empty());
}

TEST_CASE("NetworkConfig: old shape uses legacy topic root") {
    NetworkConfig config;
    REQUIRE(config.loadFromString(R"({
        "instance": "soil-probe-north",
        "location": "garden-3"
    })"));

    REQUIRE(config.getTopicRoot() == "garden-3/devices/ugly-duckling/soil-probe-north");
}

TEST_CASE("NetworkConfig: old shape without location omits location prefix from topic root") {
    NetworkConfig config;
    REQUIRE(config.loadFromString(R"({"instance": "soil-probe-north"})"));

    REQUIRE(config.getTopicRoot() == "devices/ugly-duckling/soil-probe-north");
}

TEST_CASE("NetworkConfig: old shape uses instance as hostname with colons replaced") {
    NetworkConfig config;
    REQUIRE(config.loadFromString(R"({"instance": "aa:bb:cc:dd:ee:ff"})"));

    REQUIRE(config.getHostname() == "aa-bb-cc-dd-ee-ff");
}

TEST_CASE("NetworkConfig: missing instance and location handled gracefully") {
    // A new-shape config without instance/location should not crash or produce garbage.
    NetworkConfig config;
    REQUIRE(config.loadFromString(R"({
        "id": "ABC123xyz99",
        "host": "mqtt.example.com",
        "port": 8883
    })"));

    // id drives everything — instance/location are simply absent
    REQUIRE(config.getTopicRoot() == "d/ABC123xyz99");
    REQUIRE(config.getHostname() == "ABC123xyz99");
    REQUIRE(config.instance.get().empty());
    REQUIRE(config.location.get().empty());
    REQUIRE_FALSE(config.instance.hasValue());
    REQUIRE_FALSE(config.location.hasValue());
}

TEST_CASE("NetworkConfig: completely empty config does not crash") {
    // Edge case: no id, no instance, no location. Should not happen in practice but must not crash.
    NetworkConfig config;
    REQUIRE(config.loadFromString(R"({})"));

    REQUIRE(config.id.get().empty());
    REQUIRE(config.instance.get().empty());
    REQUIRE(config.location.get().empty());
    // Legacy fallback: empty topic root (pathological but safe)
    REQUIRE(config.getTopicRoot() == "devices/ugly-duckling/");
    REQUIRE(config.getHostname().empty());
}
