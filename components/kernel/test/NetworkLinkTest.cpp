#include <NetworkLink.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace cornucopia::ugly_duckling::kernel;

TEST_CASE("chooseNetworkLink defaults to WiFi without links") {
    auto choice = chooseNetworkLink({}, true);

    REQUIRE(choice.link == NetworkLink::WiFi);
    REQUIRE_FALSE(choice.error.has_value());
}

TEST_CASE("chooseNetworkLink picks WiFi") {
    auto choice = chooseNetworkLink({ "wifi" }, true);

    REQUIRE(choice.link == NetworkLink::WiFi);
    REQUIRE_FALSE(choice.error.has_value());
}

TEST_CASE("chooseNetworkLink picks cellular when a modem is available") {
    auto choice = chooseNetworkLink({ "cellular" }, true);

    REQUIRE(choice.link == NetworkLink::Cellular);
    REQUIRE_FALSE(choice.error.has_value());
}

TEST_CASE("chooseNetworkLink falls back to WiFi when cellular is requested without a modem") {
    auto choice = chooseNetworkLink({ "cellular" }, false);

    REQUIRE(choice.link == NetworkLink::WiFi);
    REQUIRE(choice.error.has_value());
}

TEST_CASE("chooseNetworkLink falls back to WiFi on an unknown link") {
    auto choice = chooseNetworkLink({ "lora" }, true);

    REQUIRE(choice.link == NetworkLink::WiFi);
    REQUIRE(choice.error.has_value());
}

TEST_CASE("chooseNetworkLink falls back to WiFi on more than one link") {
    auto choice = chooseNetworkLink({ "cellular", "wifi" }, true);

    REQUIRE(choice.link == NetworkLink::WiFi);
    REQUIRE(choice.error.has_value());
}
