#pragma once

#include <BootMessage.hpp>
#include <NvsStore.hpp>
#include <ShutdownManager.hpp>
#include <Telemetry.hpp>
#include <devices/DeviceConfiguration.hpp>
#include <devices/DeviceDefinition.hpp>
#include <functions/Function.hpp>
#include <mqtt/MqttRoot.hpp>
#include <peripherals/Peripheral.hpp>

#include <cstdint>
#include <memory>
#include <string>

using namespace cornucopia::ugly_duckling::devices;
using namespace cornucopia::ugly_duckling::functions;
using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::mqtt;
using namespace cornucopia::ugly_duckling::peripherals;

/**
 * @brief Result of peripheral and function initialization: the managers, telemetry collector,
 * device manifest entry for SYNC, and per-peripheral/function init feedback for the BOOT message.
 */
struct DeviceRuntimeInit {
    std::shared_ptr<FunctionRegistry> functionRegistry;
    std::shared_ptr<TelemetryCollector> telemetryCollector;
    ConfigManifestEntry deviceManifestEntry;
    InitState initState = InitState::Success;
    JsonDocument peripheralsInitDoc;
    JsonDocument functionsInitDoc;
};

/**
 * @brief Creates peripheral and function infrastructure, registers factories from the device
 * definition, then runs the init loops for peripherals and functions.
 */
DeviceRuntimeInit initDeviceRuntime(
    const std::shared_ptr<I2CManager>& i2c,
    const std::shared_ptr<MqttRoot>& mqttRoot,
    const std::shared_ptr<SwitchManager>& switches,
    const std::shared_ptr<TelemetryPublisher>& telemetryPublisher,
    const std::shared_ptr<DeviceDefinition>& deviceDefinition,
    const std::shared_ptr<DeviceConfiguration>& deviceConfig,
    const std::shared_ptr<NvsStore>& configNvs,
    const std::shared_ptr<ShutdownManager>& shutdownManager,
    const ConfigManifestEntry& deviceManifestEntry);
