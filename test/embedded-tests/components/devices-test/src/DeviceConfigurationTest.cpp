#include <Pin.hpp>
#include <catch2/catch_test_macros.hpp>
#include <devices/DeviceConfiguration.hpp>

#include <string>

using namespace cornucopia::ugly_duckling::devices;

// A pin name the board doesn't have must fail loading with a message that names the property and
// the pin, rather than abort without a trace (#673).

TEST_CASE("DeviceConfiguration: known pin loads") {
    InternalPin::registerPin("TEST_NSLEEP", GPIO_NUM_4);
    DeviceConfiguration config;
    REQUIRE(config.loadFromString(R"({"motorNSleepPin": "TEST_NSLEEP"})"));

    REQUIRE(config.motorNSleepPin.get()->getName() == "TEST_NSLEEP");
}

TEST_CASE("DeviceConfiguration: unknown pin fails with the property and pin name") {
    DeviceConfiguration config;
    auto result = config.loadFromString(R"({"motorNSleepPin": "C4"})");

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message == "Invalid 'motorNSleepPin': Unknown pin: C4");
    REQUIRE_FALSE(config.motorNSleepPin.hasValue());
}
