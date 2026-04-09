#include <catch2/catch_test_macros.hpp>

#include <Strings.hpp>

using namespace cornucopia::ugly_duckling::kernel;

TEST_CASE("toHexString") {
    REQUIRE(toHexString(0) == "0x0");
    REQUIRE(toHexString(1) == "0x1");
    REQUIRE(toHexString(15) == "0xf");
    REQUIRE(toHexString(0x123456ab) == "0x123456ab");
}
