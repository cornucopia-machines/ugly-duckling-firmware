#include "BootMessage.hpp"
#include "I2CManager.hpp"
#include "NvsStore.hpp"
#include "PulseCounter.hpp"
#include "PwmManager.hpp"
#include "ShutdownManager.hpp"
#include "Telemetry.hpp"
#include "config/ConfigManifestEntry.hpp"
#include "devices/DeviceConfiguration.hpp"
#include "devices/DeviceDefinition.hpp"
#include "drivers/SwitchManager.hpp"
#include "functions/Function.hpp"
#include "mqtt/MqttRoot.hpp"
#include "peripherals/Peripheral.hpp"
#include <DeviceInit.hpp>
#include <Log.hpp>

#include <memory>
#include <utility>

using namespace cornucopia::ugly_duckling::devices;
using namespace cornucopia::ugly_duckling::functions;
using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::peripherals;

DeviceRuntimeInit initDeviceRuntime(
    const std::shared_ptr<I2CManager>& i2c,
    const std::shared_ptr<MqttRoot>& mqttRoot,
    const std::shared_ptr<SwitchManager>& switches,
    const std::shared_ptr<TelemetryPublisher>& telemetryPublisher,
    const std::shared_ptr<DeviceDefinition>& deviceDefinition,
    const std::shared_ptr<DeviceConfiguration>& deviceConfig,
    const std::shared_ptr<NvsStore>& configNvs,
    const std::shared_ptr<ShutdownManager>& shutdownManager,
    const ConfigManifestEntry& deviceManifestEntry) {

    auto peripheralsNvs = std::make_shared<NvsStore>("perf-state");
    auto pulseCounterManager = std::make_shared<PulseCounterManager>();
    auto pwm = std::make_shared<PwmManager>();
    auto telemetryCollector = std::make_shared<TelemetryCollector>();

    // Init peripherals
    auto peripheralServices = PeripheralServices {
        .i2c = i2c,
        .mqttDeviceRoot = mqttRoot,
        .nvs = peripheralsNvs,
        .pulseCounterManager = pulseCounterManager,
        .pwmManager = pwm,
        .switches = switches,
        .telemetryPublisher = telemetryPublisher,
    };
    auto peripheralManager = std::make_shared<PeripheralManager>(telemetryCollector, peripheralServices);
    shutdownManager->registerShutdownListener([peripheralManager]() {
        peripheralManager->shutdown();
    });
    deviceDefinition->registerPeripheralFactories(peripheralManager, peripheralServices, deviceConfig);

    // Init functions
    auto functionServices = FunctionServices {
        .peripherals = peripheralManager,
        .telemetryPublisher = telemetryPublisher,
    };
    // Function configuration lives in the same config slot NVS namespace ("config-<slot>") as the
    // device document -- there is no separate function-cfg namespace (docs/Configuration.md,
    // "Storage: envelopes and slots").
    auto functionRegistry = std::make_shared<FunctionRegistry>(configNvs, functionServices);
    shutdownManager->registerShutdownListener([functionRegistry]() {
        functionRegistry->shutdown();
    });
    deviceDefinition->registerFunctionFactories(functionRegistry);

    InitState initState = InitState::Success;

    // Init peripherals
    JsonDocument peripheralsInitDoc;
    auto peripheralsInitJson = peripheralsInitDoc.to<JsonArray>();

    const auto& peripheralsSettings = deviceConfig->peripherals.get();
    LOGI("Loading configuration for %d peripherals",
        peripheralsSettings.size());
    for (const auto& peripheralSettings : peripheralsSettings) {
        if (!peripheralManager->createPeripheral(peripheralSettings.get(), peripheralsInitJson)) {
            initState = InitState::PeripheralError;
        }
    }

    // Start ULP pulse counter after all channels have been registered by peripherals above.
    pulseCounterManager->start();

    JsonDocument functionsInitDoc;
    auto functionsInitJson = functionsInitDoc.to<JsonArray>();
    const auto& functionsSettings = deviceConfig->functions.get();
    LOGI("Loading configuration for %d functions",
        functionsSettings.size());
    for (const auto& functionSettings : functionsSettings) {
        if (!functionRegistry->createFunction(functionSettings.get(), functionsInitJson)) {
            initState = InitState::FunctionError;
        }
    }

    return {
        .functionRegistry = functionRegistry,
        .telemetryCollector = telemetryCollector,
        .deviceManifestEntry = deviceManifestEntry,
        .initState = initState,
        .peripheralsInitDoc = std::move(peripheralsInitDoc),
        .functionsInitDoc = std::move(functionsInitDoc),
    };
}
