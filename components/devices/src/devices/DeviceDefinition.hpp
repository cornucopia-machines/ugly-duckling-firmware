#pragma once

#include <list>
#include <memory>
#include <string>

#include <Pin.hpp>

#include <ArduinoJson.h>

#include <devices/DeviceSettings.hpp>

#include <Log.hpp>
#include <PcntManager.hpp>
#include <PulseCounter.hpp>
#include <PwmManager.hpp>
#include <drivers/BatteryDriver.hpp>
#include <drivers/LedDriver.hpp>

#include <functions/chicken_door/ChickenDoor.hpp>
#include <functions/plot_controller/PlotController.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/analog_meter/AnalogMeter.hpp>
#include <peripherals/environment/Ds18B20SoilSensor.hpp>
#include <peripherals/environment/Environment.hpp>
#include <peripherals/environment/Fdc1004SoilMoistureSensor.hpp>
#include <peripherals/environment/KalmanFilterSoilSensor.hpp>
#include <peripherals/environment/NtcTemperatureSensor.hpp>
#include <peripherals/environment/Sht2xSensor.hpp>
#include <peripherals/environment/Sht3xSensor.hpp>
#include <peripherals/environment/SoilMoistureSensor.hpp>
#include <peripherals/fence/ElectricFenceMonitor.hpp>
#include <peripherals/flow_meter/FlowMeter.hpp>
#include <peripherals/light_sensor/AnalogLightSensor.hpp>
#include <peripherals/light_sensor/Bh1750.hpp>
#include <peripherals/light_sensor/Tsl2591.hpp>
#include <peripherals/multiplexer/Xl9535.hpp>

using namespace farmhub::functions;
using namespace farmhub::kernel;
using namespace farmhub::kernel::drivers;
using namespace farmhub::peripherals;

namespace farmhub::devices {

#define UD_DEFINE_PIN3(GPIO, VAR, STR) \
    const InternalPinPtr VAR = InternalPin::registerPin(STR, GPIO);

#define UD_DEFINE_PIN2(GPIO, VAR) \
    UD_DEFINE_PIN3(GPIO, VAR, #VAR)

#define UD_GET_MACRO(_1, _2, _3, NAME, ...) NAME

#define DEFINE_PIN(...) UD_GET_MACRO(__VA_ARGS__, UD_DEFINE_PIN3, UD_DEFINE_PIN2)(__VA_ARGS__)

struct DeviceConfig {
    std::string model;
    int revision;
    gpio_num_t boot;
    gpio_num_t status;
};

class DeviceDefinition {
public:
    explicit DeviceDefinition(DeviceConfig config)
        : model(std::move(config.model))
        , revision(config.revision)
        , bootPin(InternalPin::registerPin("BOOT", config.boot))
        , statusPin(InternalPin::registerPin("STATUS", config.status)) {
    }

    virtual ~DeviceDefinition() = default;

    void registerPeripheralFactories(const std::shared_ptr<PeripheralManager>& peripheralManager, const PeripheralServices& services, const std::shared_ptr<DeviceSettings>& settings) {
        peripheralManager->registerFactory(environment::makeFactoryForSht3x());
        // TODO Unify these two factories
        peripheralManager->registerFactory(environment::makeFactoryForSht2x("sht2x"));
        peripheralManager->registerFactory(environment::makeFactoryForSht2x("htu2x"));
        peripheralManager->registerFactory(environment::makeFactoryForNtcTemperatureSensor());

        peripheralManager->registerFactory(environment::makeFactoryForSoilMoisture());
        peripheralManager->registerFactory(environment::makeFactoryForFdc1004SoilMoisture());
        peripheralManager->registerFactory(environment::makeFactoryForDs18b20());
        peripheralManager->registerFactory(environment::makeFactoryForKalmanSoilMoisture());

        peripheralManager->registerFactory(fence::makeFactory());

        peripheralManager->registerFactory(light_sensor::makeFactoryForAnalogLightSensor());
        peripheralManager->registerFactory(light_sensor::makeFactoryForBh1750());
        peripheralManager->registerFactory(light_sensor::makeFactoryForTsl2591());

        peripheralManager->registerFactory(multiplexer::makeFactoryForXl9535());

        peripheralManager->registerFactory(analog_meter::makeFactory());
        peripheralManager->registerFactory(flow_meter::makeFactory());

        registerDeviceSpecificPeripheralFactories(peripheralManager, services, settings);
    }

    void registerFunctionFactories(const std::shared_ptr<FunctionManager>& functionManager) {
        functionManager->registerFactory(plot_controller::makeFactory());
        functionManager->registerFactory(chicken_door::makeFactory());
    }

    /**
     * @brief Returns zero or more JSON configurations for any built-in peripheral of the device.
     */
    virtual std::list<std::string> getBuiltInPeripherals() {
        return {};
    }

    virtual std::shared_ptr<BatteryDriver> createBatteryDriver(const std::shared_ptr<I2CManager>& /*i2c*/) {
        return nullptr;
    }

    const std::string model;
    const int revision;
    const InternalPinPtr bootPin;
    const InternalPinPtr statusPin;

protected:
    virtual void registerDeviceSpecificPeripheralFactories(const std::shared_ptr<PeripheralManager>& peripheralManager, const PeripheralServices& services, const std::shared_ptr<DeviceSettings>& /*settings*/) {
    }
};

}    // namespace farmhub::devices
