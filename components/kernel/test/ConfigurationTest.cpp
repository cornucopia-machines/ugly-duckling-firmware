#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <chrono>
#include <string>

#include <Configuration.hpp>

using namespace std::chrono;
using namespace std::chrono_literals;
using namespace cornucopia::ugly_duckling::kernel;

struct TestNestedConfig : ConfigurationSection {
    Property<int> intValue { this, "intValue" };
};

struct TestConfig : ConfigurationSection {
    Property<int> intValue { this, "intValue" };
    Property<std::string> stringValue { this, "stringValue" };
    Property<bool> boolValue { this, "boolValue" };
    Property<seconds> secondsValue { this, "secondsValue" };
    NamedConfigurationEntry<TestNestedConfig> nested { this, "nested" };
};

std::string toString(const ConfigurationEntry& config) {
    JsonDocument json;
    auto root = json.to<JsonObject>();
    config.store(root);
    std::string jsonString;
    serializeJson(json, jsonString);
    return jsonString;
}

TEST_CASE("empty configuration is stored as empty JSON") {
    TestConfig config;
    REQUIRE(toString(config) == R"({})");
    REQUIRE(!config.intValue.hasValue());
    REQUIRE(config.intValue.get() == 0);
    REQUIRE(!config.stringValue.hasValue());
    REQUIRE(config.stringValue.get() == "");
    REQUIRE(!config.boolValue.hasValue());
    REQUIRE(config.boolValue.get() == false);
    REQUIRE(!config.secondsValue.hasValue());
    REQUIRE(config.secondsValue.get() == 0s);
    REQUIRE(!config.nested.hasValue());
    REQUIRE(!config.nested.get()->intValue.hasValue());
    REQUIRE(config.nested.get()->intValue.get() == 0);
}

TEST_CASE("empty configuration can be loaded from empty JSON") {
    TestConfig config;
    config.loadFromString(R"({})");
    REQUIRE(!config.intValue.hasValue());
    REQUIRE(config.intValue.get() == 0);
    REQUIRE(!config.stringValue.hasValue());
    REQUIRE(config.stringValue.get() == "");
    REQUIRE(!config.boolValue.hasValue());
    REQUIRE(config.boolValue.get() == false);
    REQUIRE(!config.secondsValue.hasValue());
    REQUIRE(config.secondsValue.get() == 0s);
    REQUIRE(!config.nested.hasValue());
    REQUIRE(!config.nested.get()->intValue.hasValue());
    REQUIRE(config.nested.get()->intValue.get() == 0);
}

TEST_CASE("empty configuration can be loaded from JSON with null values") {
    TestConfig config;
    config.loadFromString(R"({"intValue":null,"stringValue":null,"boolValue":null,"secondsValue":null,"nested":null})");
    REQUIRE(!config.intValue.hasValue());
    REQUIRE(config.intValue.get() == 0);
    REQUIRE(!config.stringValue.hasValue());
    REQUIRE(config.stringValue.get() == "");
    REQUIRE(!config.boolValue.hasValue());
    REQUIRE(config.boolValue.get() == false);
    REQUIRE(!config.secondsValue.hasValue());
    REQUIRE(config.secondsValue.get() == 0s);
    REQUIRE(!config.nested.hasValue());
    REQUIRE(!config.nested.get()->intValue.hasValue());
    REQUIRE(config.nested.get()->intValue.get() == 0);
}

TEST_CASE("configuration can be loaded from JSON with invalid values") {
    TestConfig config;
    config.loadFromString(R"({"intValue":"123","stringValue":123,"boolValue":"false","secondsValue":"60","nested":"{}"})");
    REQUIRE(!config.intValue.hasValue());
    REQUIRE(config.intValue.get() == 0);
    REQUIRE(!config.stringValue.hasValue());
    REQUIRE(config.stringValue.get() == "");
    REQUIRE(!config.boolValue.hasValue());
    REQUIRE(config.boolValue.get() == false);
    REQUIRE(!config.secondsValue.hasValue());
    REQUIRE(config.secondsValue.get() == 0s);
    REQUIRE(!config.nested.hasValue());
    REQUIRE(!config.nested.get()->intValue.hasValue());
    REQUIRE(config.nested.get()->intValue.get() == 0);
}

TEST_CASE("configuration with values is loaded from JSON and is stored as JSON") {
    TestConfig config;
    config.loadFromString(R"({"intValue":42,"stringValue":"hello","boolValue":true,"secondsValue":60,"nested":{"intValue":7}})");
    REQUIRE(config.intValue.hasValue());
    REQUIRE(config.intValue.get() == 42);
    REQUIRE(config.stringValue.hasValue());
    REQUIRE(config.stringValue.get() == "hello");
    REQUIRE(config.boolValue.hasValue());
    REQUIRE(config.boolValue.get() == true);
    REQUIRE(config.secondsValue.hasValue());
    REQUIRE(config.secondsValue.get() == 60s);
    REQUIRE(config.nested.hasValue());
    REQUIRE(config.nested.get()->intValue.hasValue());
    REQUIRE(config.nested.get()->intValue.get() == 7);
    REQUIRE(toString(config) == R"({"intValue":42,"stringValue":"hello","boolValue":true,"secondsValue":60,"nested":{"intValue":7}})");
}

struct TestNestedConfigWithDefaults : ConfigurationSection {
    Property<int> intValue { this, "intValue", 100 };
};

struct TestConfigWithDefaults : public ConfigurationSection {
    Property<int> intValue { this, "intValue", 42 };
    Property<std::string> stringValue { this, "stringValue", "default" };
    Property<bool> boolValue { this, "boolValue", true };
    Property<std::chrono::seconds> secondsValue { this, "secondsValue", 30s };
    NamedConfigurationEntry<TestNestedConfigWithDefaults> nested { this, "nested" };
};

TEST_CASE("configuration with default values loaded from empty JSON has default values") {
    TestConfigWithDefaults config;
    REQUIRE(toString(config) == R"({})");
    REQUIRE(!config.intValue.hasValue());
    REQUIRE(config.intValue.get() == 42);
    REQUIRE(!config.stringValue.hasValue());
    REQUIRE(config.stringValue.get() == "default");
    REQUIRE(!config.boolValue.hasValue());
    REQUIRE(config.boolValue.get() == true);
    REQUIRE(!config.secondsValue.hasValue());
    REQUIRE(config.secondsValue.get() == 30s);
    REQUIRE(!config.nested.hasValue());
    REQUIRE(!config.nested.get()->intValue.hasValue());
    REQUIRE(config.nested.get()->intValue.get() == 100);
}

TEST_CASE("configuration with default values loaded from non-empty JSON has actual values") {
    TestConfigWithDefaults config;
    config.loadFromString(R"({"intValue": 100, "stringValue": "custom", "boolValue": false, "secondsValue": 45, "nested": {"intValue": 200}})");
    REQUIRE(toString(config) == R"({"intValue":100,"stringValue":"custom","boolValue":false,"secondsValue":45,"nested":{"intValue":200}})");
    REQUIRE(config.intValue.hasValue());
    REQUIRE(config.intValue.get() == 100);
    REQUIRE(config.stringValue.hasValue());
    REQUIRE(config.stringValue.get() == "custom");
    REQUIRE(config.boolValue.hasValue());
    REQUIRE(config.boolValue.get() == false);
    REQUIRE(config.secondsValue.hasValue());
    REQUIRE(config.secondsValue.get() == 45s);
    REQUIRE(config.nested.hasValue());
    REQUIRE(config.nested.get()->intValue.hasValue());
    REQUIRE(config.nested.get()->intValue.get() == 200);
}

TEST_CASE("throws on invalid JSON") {
    TestConfig config;
    REQUIRE_THROWS_MATCHES(
        config.loadFromString("NOT JSON"),
        ConfigurationException,
        Catch::Matchers::Message("ConfigurationException: Cannot parse JSON configuration: InvalidInput: NOT JSON"));
}
