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

using namespace farmhub::kernel;
using namespace farmhub::kernel::drivers;
using namespace farmhub::peripherals;
using namespace farmhub::peripherals::api;

namespace farmhub::peripherals::valve {

inline PeripheralFactory makeFactory(
    const std::map<std::string, std::shared_ptr<PwmMotorDriver>>& motors,
    ValveControlStrategyType defaultStrategy) {

    return makePeripheralFactory<IValve, Valve, ValveSettings>(
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
        defaultStrategy);
}

}    // namespace farmhub::peripherals::valve
