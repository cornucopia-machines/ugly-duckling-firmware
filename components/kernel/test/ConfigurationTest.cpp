#include <catch2/catch_test_macros.hpp>
#include <config/Configuration.hpp>

#include <chrono>
#include <string>

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

TEST_CASE("empty configuration has defaults and no values") {
    TestConfig config;
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
    REQUIRE(config.loadFromString(R"({})"));
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
    REQUIRE(config.loadFromString(R"({"intValue":null,"stringValue":null,"boolValue":null,"secondsValue":null,"nested":null})"));
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
    REQUIRE(config.loadFromString(R"({"intValue":"123","stringValue":123,"boolValue":"false","secondsValue":"60","nested":"{}"})"));
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

TEST_CASE("configuration with values is loaded from JSON") {
    TestConfig config;
    REQUIRE(config.loadFromString(R"({"intValue":42,"stringValue":"hello","boolValue":true,"secondsValue":60,"nested":{"intValue":7}})"));
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
    REQUIRE(config.loadFromString(R"({"intValue": 100, "stringValue": "custom", "boolValue": false, "secondsValue": 45, "nested": {"intValue": 200}})"));
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

TEST_CASE("fails on invalid JSON") {
    TestConfig config;
    auto result = config.loadFromString("NOT JSON");
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message == "Cannot parse JSON configuration: InvalidInput: NOT JSON");
}

struct EvenNumber {
    int value;
};

namespace ArduinoJson {
template <>
struct Converter<EvenNumber> {
    static EvenNumber fromJson(JsonVariantConst src) {
        return { src.as<int>() };
    }

    static bool checkJson(JsonVariantConst src) {
        return src.is<int>();
    }
};
}    // namespace ArduinoJson

template <>
struct cornucopia::ugly_duckling::kernel::config::PropertyValidator<EvenNumber> {
    static Result<> validate(const EvenNumber& number, JsonVariantConst /*json*/) {
        if (number.value % 2 != 0) {
            return failure(std::to_string(number.value) + " is odd");
        }
        return {};
    }
};

struct TestValidatedNestedConfig : ConfigurationSection {
    Property<EvenNumber> even { this, "even" };
};

struct TestValidatedConfig : ConfigurationSection {
    Property<EvenNumber> even { this, "even" };
    ArrayProperty<EvenNumber> evens { this, "evens" };
    NamedConfigurationEntry<TestValidatedNestedConfig> nested { this, "nested" };
};

TEST_CASE("validated property accepts a valid value") {
    TestValidatedConfig config;
    REQUIRE(config.loadFromString(R"({"even": 2, "evens": [4, 6], "nested": {"even": 8}})"));
    REQUIRE(config.even.get().value == 2);
    REQUIRE(config.evens.get().size() == 2);
    REQUIRE(config.nested.get()->even.get().value == 8);
}

TEST_CASE("validated property fails on an invalid value, naming the property") {
    TestValidatedConfig config;
    auto result = config.loadFromString(R"({"even": 3})");
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message == "Invalid 'even': 3 is odd");
    REQUIRE_FALSE(config.even.hasValue());
}

TEST_CASE("validated array property fails on an invalid entry") {
    TestValidatedConfig config;
    auto result = config.loadFromString(R"({"evens": [2, 5]})");
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message == "Invalid entry in 'evens': 5 is odd");
}

TEST_CASE("validated property in a nested section fails with the section's name") {
    TestValidatedConfig config;
    auto result = config.loadFromString(R"({"nested": {"even": 7}})");
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message == "In 'nested': Invalid 'even': 7 is odd");
}
