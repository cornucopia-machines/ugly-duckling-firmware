#pragma once

#include <chrono>
#include <memory>

#include <ArduinoJson.h>

#include <Configuration.hpp>
#include <Task.hpp>
#include <Telemetry.hpp>
#include <drivers/MotorDriver.hpp>

#include <peripherals/Motors.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/api/IValve.hpp>
#include <peripherals/valve/Valve.hpp>
#include <peripherals/valve/ValveSettings.hpp>

using namespace std::chrono;
using namespace std::chrono_literals;

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::drivers;
using namespace cornucopia::ugly_duckling::peripherals;
using namespace cornucopia::ugly_duckling::peripherals::api;

namespace cornucopia::ugly_duckling::peripherals::valve {

inline PeripheralFactory makeFactory(
    const std::map<std::string, std::shared_ptr<PwmMotorDriver>>& motors,
    ValveControlStrategyType defaultStrategy) {

    return makePeripheralFactory<Valve, ValveSettings, IValve>(
        "valve",
        "valve",
        [motors](PeripheralInitParameters& params, const std::shared_ptr<ValveSettings>& settings) {
            auto strategy = settings->createValveControlStrategy(motors, settings->motor.get());
            auto valve = std::make_shared<Valve>(
                params.name,
                std::move(strategy),
                params.services.nvs);

            params.registerFeature("valve", [valve](JsonObject& telemetry) {
                valve->populateTelemetry(telemetry);
            });

            return valve;
        },
        [defaultStrategy] { return std::make_shared<ValveSettings>(defaultStrategy); });
}

}    // namespace cornucopia::ugly_duckling::peripherals::valve
