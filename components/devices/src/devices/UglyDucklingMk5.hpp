#pragma once

#include <Pin.hpp>
#include <drivers/BatteryDriver.hpp>
#include <drivers/Drv8874Driver.hpp>
#include <drivers/LedDriver.hpp>

#include <peripherals/door/Door.hpp>
#include <peripherals/valve/ValveFactory.hpp>

#include <devices/DeviceDefinition.hpp>

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::peripherals::door;
using namespace cornucopia::ugly_duckling::peripherals::valve;

namespace cornucopia::ugly_duckling::devices {

class UglyDucklingMk5 : public DeviceDefinition {
public:
    UglyDucklingMk5()
        : DeviceDefinition({ .model = "mk5", .revision = 2, .boot = GPIO_NUM_0, .status = GPIO_NUM_2 }) {
    }

protected:
    void registerDeviceSpecificPeripheralFactories(const std::shared_ptr<PeripheralManager>& peripheralManager, const PeripheralServices& services, const std::shared_ptr<DeviceSettings>& _settings) override {
        auto motorA = std::make_shared<Drv8874Driver>(
            services.pwmManager,
            AIN1,
            AIN2,
            AIPROPI,
            NFault,
            NSLEEP);

        auto motorB = std::make_shared<Drv8874Driver>(
            services.pwmManager,
            BIN1,
            BIN2,
            BIPROPI,
            NFault,
            NSLEEP);

        std::map<std::string, std::shared_ptr<PwmMotorDriver>> motors = { { "a", motorA }, { "b", motorB } };

        peripheralManager->registerFactory(valve::makeFactory(motors, ValveControlStrategyType::Latching));
        peripheralManager->registerFactory(door::makeFactory(motors));
    }

private:
    DEFINE_PIN(GPIO_NUM_1, BATTERY)
    DEFINE_PIN(GPIO_NUM_4, AIPROPI)
    DEFINE_PIN(GPIO_NUM_5, IOA1, "A1")
    DEFINE_PIN(GPIO_NUM_6, IOA2, "A2")
    DEFINE_PIN(GPIO_NUM_7, BIPROPI)
    DEFINE_PIN(GPIO_NUM_15, IOB1, "B1")
    DEFINE_PIN(GPIO_NUM_16, AIN1)
    DEFINE_PIN(GPIO_NUM_17, AIN2)
    DEFINE_PIN(GPIO_NUM_18, BIN1)
    DEFINE_PIN(GPIO_NUM_8, BIN2)
    DEFINE_PIN(GPIO_NUM_19, DMINUS, "D-")
    DEFINE_PIN(GPIO_NUM_20, DPLUS, "D+")
    DEFINE_PIN(GPIO_NUM_9, IOB2, "B2")
    DEFINE_PIN(GPIO_NUM_10, NSLEEP)
    DEFINE_PIN(GPIO_NUM_11, NFault)
    DEFINE_PIN(GPIO_NUM_12, IOC4, "C4")
    DEFINE_PIN(GPIO_NUM_13, IOC3, "C3")
    DEFINE_PIN(GPIO_NUM_14, IOC2, "C2")
    DEFINE_PIN(GPIO_NUM_21, IOC1, "C1")
    DEFINE_PIN(GPIO_NUM_47, IOD4, "D4")
    DEFINE_PIN(GPIO_NUM_48, IOD3, "D3")
    DEFINE_PIN(GPIO_NUM_35, SDA)
    DEFINE_PIN(GPIO_NUM_36, SCL)
    DEFINE_PIN(GPIO_NUM_37, IOD1, "D1")
    DEFINE_PIN(GPIO_NUM_38, IOD2, "D2")
    DEFINE_PIN(GPIO_NUM_39, TCK)
    DEFINE_PIN(GPIO_NUM_40, TDO)
    DEFINE_PIN(GPIO_NUM_41, TDI)
    DEFINE_PIN(GPIO_NUM_42, TMS)
    DEFINE_PIN(GPIO_NUM_44, RXD0)
    DEFINE_PIN(GPIO_NUM_43, TXD0)
};

}    // namespace cornucopia::ugly_duckling::devices
