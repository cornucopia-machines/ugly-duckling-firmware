#include <catch2/catch_test_macros.hpp>

#include <Strings.hpp>

TEST_CASE("toHexString") {
    REQUIRE(farmhub::kernel::toHexString(0) == "0x0");
    REQUIRE(farmhub::kernel::toHexString(1) == "0x1");
    REQUIRE(farmhub::kernel::toHexString(15) == "0xf");
    REQUIRE(farmhub::kernel::toHexString(0x123456ab) == "0x123456ab");
}
