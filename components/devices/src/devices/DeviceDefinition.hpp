#pragma once

#include <Log.hpp>
#include <Pin.hpp>
#include <PulseCounter.hpp>
#include <PwmManager.hpp>
#include <devices/DeviceConfiguration.hpp>
#include <drivers/BatteryDriver.hpp>
#include <drivers/Ina219Driver.hpp>
#include <drivers/LedDriver.hpp>
#include <drivers/cellular/CellularModemPins.hpp>
#include <functions/chicken_door/ChickenDoor.hpp>
#include <functions/plot_controller/PlotController.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/analog_meter/AnalogMeter.hpp>
#include <peripherals/environment/ChirpSoilSensor.hpp>
#include <peripherals/environment/Ds18B20SoilSensor.hpp>
#include <peripherals/environment/Environment.hpp>
#include <peripherals/environment/Hdc2010Sensor.hpp>
#include <peripherals/environment/Hw390SoilMoistureSensor.hpp>
#include <peripherals/environment/KalmanFilterSoilSensor.hpp>
#include <peripherals/environment/NtcTemperatureSensor.hpp>
#include <peripherals/environment/Sht2xSensor.hpp>
#include <peripherals/environment/Sht3xSensor.hpp>
#include <peripherals/environment/SpadefootToadSensor.hpp>
#include <peripherals/fence/ElectricFenceMonitor.hpp>
#include <peripherals/flow_meter/FlowMeter.hpp>
#include <peripherals/light_sensor/AnalogLightSensor.hpp>
#include <peripherals/light_sensor/Bh1750.hpp>
#include <peripherals/light_sensor/Tsl2591.hpp>
#include <peripherals/multiplexer/Xl9535.hpp>

#include <ArduinoJson.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace cornucopia::ugly_duckling::functions;
using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::drivers;
using namespace cornucopia::ugly_duckling::peripherals;

namespace cornucopia::ugly_duckling::devices {

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

    void registerPeripheralFactories(const std::shared_ptr<PeripheralManager>& peripheralManager, const PeripheralServices& services, const std::shared_ptr<DeviceConfiguration>& deviceConfig) {
        peripheralManager->registerFactory(environment::makeFactoryForHdc2010());
        peripheralManager->registerFactory(environment::makeFactoryForSht3x());
        // TODO Unify these two factories
        peripheralManager->registerFactory(environment::makeFactoryForSht2x("sht2x"));
        peripheralManager->registerFactory(environment::makeFactoryForSht2x("htu2x"));
        peripheralManager->registerFactory(environment::makeFactoryForNtcTemperatureSensor());

        peripheralManager->registerFactory(environment::makeFactoryForHw390SoilMoisture());
        // For backward compatibility with existing configs; can be removed after a while
        peripheralManager->registerFactory(environment::makeFactoryForHw390SoilMoisture("environment:soil-moisture"));
        peripheralManager->registerFactory(environment::makeFactoryForChirpSoilSensor());
        peripheralManager->registerFactory(environment::makeFactoryForSpadefootToadSensor());
        peripheralManager->registerFactory(environment::makeFactoryForDs18b20());
        peripheralManager->registerFactory(environment::makeFactoryForKalmanSoilMoisture());

        peripheralManager->registerFactory(fence::makeFactory());

        peripheralManager->registerFactory(light_sensor::makeFactoryForAnalogLightSensor());
        peripheralManager->registerFactory(light_sensor::makeFactoryForBh1750());
        peripheralManager->registerFactory(light_sensor::makeFactoryForTsl2591());

        peripheralManager->registerFactory(multiplexer::makeFactoryForXl9535());

        peripheralManager->registerFactory(analog_meter::makeFactory());
        peripheralManager->registerFactory(flow_meter::makeFactory());

        registerDeviceSpecificPeripheralFactories(peripheralManager, services, deviceConfig);
    }

    void registerFunctionFactories(const std::shared_ptr<FunctionRegistry>& functionRegistry) {
        functionRegistry->registerFactory(plot_controller::makeFactory());
        functionRegistry->registerFactory(chicken_door::makeFactory());
        registerDeviceSpecificFunctionFactories(functionRegistry);
    }

    virtual Result<std::shared_ptr<BatteryDriver>> createBatteryDriver(const std::shared_ptr<I2CManager>& _i2c) {
        return nullptr;
    }

    virtual void handleShortButtonPress(milliseconds duration) {
    }

    /**
     * @brief How the NB-IoT modem connector is wired, on boards that have one.
     */
    virtual std::optional<cellular::CellularModemPins> getCellularModemPins() const {
        return std::nullopt;
    }

    const std::string model;
    const int revision;
    const InternalPinPtr bootPin;
    const InternalPinPtr statusPin;

protected:
    virtual void registerDeviceSpecificPeripheralFactories(const std::shared_ptr<PeripheralManager>& peripheralManager, const PeripheralServices& services, const std::shared_ptr<DeviceConfiguration>& _deviceConfig) {
    }

    virtual void registerDeviceSpecificFunctionFactories(const std::shared_ptr<FunctionRegistry>& _functionRegistry) {
    }

    /**
     * @brief Calibrates the on-board INA219 current sensor (MK8 to MK10) and powers it down until needed.
     *
     * @return nullptr if the sensor doesn't respond: a current sensor we cannot talk to must not keep
     * the device from booting.
     */
    static std::shared_ptr<Ina219Driver> createOnboardIna219(const std::shared_ptr<I2CManager>& i2c, const InternalPinPtr& sda, const InternalPinPtr& scl) {
        auto ina219 = Ina219Driver::create(
            i2c,
            I2CConfig {
                .address = Ina219Driver::DEFAULT_ADDRESS,
                .sda = sda,
                .scl = scl,
            },
            Ina219Parameters {
                .uRange = INA219_BUS_RANGE_16V,
                .gain = INA219_GAIN_0_125,
                .uResolution = INA219_RES_12BIT_1S,
                .iResolution = INA219_RES_12BIT_1S,
                .mode = INA219_MODE_CONT_SHUNT_BUS,
                .shuntMilliOhm = 50,
            });
        if (!ina219) {
            LOGE("Failed to initialize INA219, continuing without it: %s", ina219.error().message.c_str());
            return nullptr;
        }
        return *ina219;
    }
};

}    // namespace cornucopia::ugly_duckling::devices
