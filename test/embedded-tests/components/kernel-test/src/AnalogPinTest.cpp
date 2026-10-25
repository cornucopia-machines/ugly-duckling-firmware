#include <Pin.hpp>
#include <catch2/catch_test_macros.hpp>

#include <soc/gpio_num.h>

using namespace cornucopia::ugly_duckling::kernel;

// GPIO 1 and 2 are both on ADC1 of the ESP32-S3, so the two pins share a unit handle
TEST_CASE("destroying an analog pin keeps the other pins on its ADC unit working") {
    auto first = AnalogPin::create(InternalPin::byGpio(GPIO_NUM_1));
    REQUIRE(first.has_value());
    auto second = AnalogPin::create(InternalPin::byGpio(GPIO_NUM_2));
    REQUIRE(second.has_value());

    first->reset();
    REQUIRE((*second)->tryAnalogRead().has_value());

    auto recreated = AnalogPin::create(InternalPin::byGpio(GPIO_NUM_1));
    REQUIRE(recreated.has_value());
    REQUIRE((*recreated)->tryAnalogRead().has_value());
}
