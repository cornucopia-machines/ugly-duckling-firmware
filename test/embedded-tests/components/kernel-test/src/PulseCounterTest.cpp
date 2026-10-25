#include <Pin.hpp>
#include <PulseCounter.hpp>
#include <catch2/catch_test_macros.hpp>

#include <driver/gpio.h>
#include <esp_err.h>
#include <soc/gpio_num.h>

using namespace cornucopia::ugly_duckling::kernel;

// GPIO 40 and 41 are not RTC GPIOs on the ESP32-S3, so they get interrupt-based counters
TEST_CASE("pulse counter manager does not count failed light sleep callback registration as done") {
    esp_err_t err = gpio_install_isr_service(0);
    REQUIRE((err == ESP_OK || err == ESP_ERR_INVALID_STATE));

    // Registers the light sleep callbacks. Kept for the rest of the run, as the callbacks point to it
    static PulseCounterManager first;
    REQUIRE(first.create({ .pin = InternalPin::byGpio(GPIO_NUM_40) }).has_value());

    // The same callbacks can't be registered twice, so registration fails for a second manager
    PulseCounterManager second;
    REQUIRE_FALSE(second.create({ .pin = InternalPin::byGpio(GPIO_NUM_41) }).has_value());
    // Without the callbacks the counter would miss pulses around light sleep, so it must not be created
    REQUIRE_FALSE(second.create({ .pin = InternalPin::byGpio(GPIO_NUM_41) }).has_value());
}
