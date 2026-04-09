#include <catch2/catch_test_macros.hpp>

#include <optional>

#include <freertos/FreeRTOS.h>

#include <Watchdog.hpp>

using namespace cornucopia::ugly_duckling::kernel;

TEST_CASE("can start and stop watchdog") {
    std::optional<WatchdogState> watchdogState;
    Watchdog watchdog("test", 1h, false, [&](WatchdogState state) {
        watchdogState = state;
    });
    REQUIRE(!watchdogState.has_value());

    watchdog.restart();
    REQUIRE(watchdogState.value() == WatchdogState::Started);

    watchdog.cancel();
    REQUIRE(watchdogState.value() == WatchdogState::Cancelled);

    watchdog.restart();
    REQUIRE(watchdogState.value() == WatchdogState::Started);

    watchdog.cancel();
    REQUIRE(watchdogState.value() == WatchdogState::Cancelled);
}

TEST_CASE("can create auto-starting watchdog") {
    std::optional<WatchdogState> watchdogState;
    Watchdog watchdog("test", 1h, true, [&](WatchdogState state) {
        watchdogState = state;
    });
    REQUIRE(watchdogState.value() == WatchdogState::Started);

    watchdog.cancel();
    REQUIRE(watchdogState.value() == WatchdogState::Cancelled);
}

TEST_CASE("watchdog can time out") {
    std::optional<WatchdogState> watchdogState;
    Watchdog watchdog("test", 10ms, false, [&](WatchdogState state) {
        watchdogState = state;
    });
    REQUIRE(!watchdogState.has_value());

    watchdog.restart();
    vTaskDelay(20 / portTICK_PERIOD_MS);
    REQUIRE(watchdogState.value() == WatchdogState::TimedOut);
}

TEST_CASE("deleting watchdog cancels") {
    std::optional<WatchdogState> watchdogState;
    {
        Watchdog watchdog("test", 1h, true, [&](WatchdogState state) {
            watchdogState = state;
        });
        REQUIRE(watchdogState.value() == WatchdogState::Started);
    }

    REQUIRE(watchdogState.value() == WatchdogState::Cancelled);
}
