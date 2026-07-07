#pragma once

#include <memory>

#include <MacAddress.hpp>
#include <Pin.hpp>
#include <soc/rtc.h>
#include <drivers/Bq27220Driver.hpp>
#include <drivers/Drv8848Driver.hpp>
#include <drivers/LedDriver.hpp>

#include <peripherals/Peripheral.hpp>
#include <peripherals/door/Door.hpp>
#include <peripherals/environment/SpadefootToadSensor.hpp>
#include <peripherals/valve/ValveFactory.hpp>

#include <devices/DeviceDefinition.hpp>

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::drivers;
using namespace cornucopia::ugly_duckling::peripherals::door;
using namespace cornucopia::ugly_duckling::peripherals::environment;
using namespace cornucopia::ugly_duckling::peripherals::valve;

namespace cornucopia::ugly_duckling::devices {

class UglyDucklingMk11Rev1 : public DeviceDefinition {
public:
    explicit UglyDucklingMk11Rev1()
        : DeviceDefinition({ .model = "mk11", .revision = 1, .boot = GPIO_NUM_9, .status = GPIO_NUM_8 }) {
        rtc_clk_32k_enable(true);
    }

    std::shared_ptr<BatteryDriver> createBatteryDriver(const std::shared_ptr<I2CManager>& _i2c) override {
        return std::make_shared<AnalogBatteryDriver>(
            BATTERY,
            2,
            BatteryParameters {
                .maximumVoltage = 4100,
                .bootThreshold = 3300,
                .shutdownThreshold = 3100,
            });
    }

protected:
    void registerDeviceSpecificPeripheralFactories(const std::shared_ptr<PeripheralManager>& peripheralManager, const PeripheralServices& services, const std::shared_ptr<DeviceSettings>& /*settings*/) override {
        auto motorDriver = Drv8848Driver::create(
            services.pwmManager,
            DAIN1,
            DAIN2,
            DBIN1,
            DBIN2,
            NFAULT,
            LOADEN);

        std::map<std::string, std::shared_ptr<PwmMotorDriver>> motors = { { "a", motorDriver->getMotorA() }, { "b", motorDriver->getMotorB() } };

        peripheralManager->registerFactory(valve::makeFactory(motors, ValveControlStrategyType::Latching));
        peripheralManager->registerFactory(door::makeFactory(motors));
        peripheralManager->registerFactory(environment::makeFactoryForSpadefootToadSensor());
    }

    // 32 kHz external crystal
    DEFINE_PIN(GPIO_NUM_0, XTAL32K_P, "XTAL_32K_P")
    DEFINE_PIN(GPIO_NUM_1, XTAL32K_N, "XTAL_32K_N")

    // Battery fuel gauge interrupt
    DEFINE_PIN(GPIO_NUM_2, BATTERY)

    // Flow meter A
    DEFINE_PIN(GPIO_NUM_3, IFLOWA)

    // Internal I2C
    DEFINE_PIN(GPIO_NUM_6, SDA)
    DEFINE_PIN(GPIO_NUM_7, SCL)

    // Status LED 2
    DEFINE_PIN(GPIO_NUM_9, STATUS2)

    // External I2C
    DEFINE_PIN(GPIO_NUM_10, EXT_SCL)
    DEFINE_PIN(GPIO_NUM_11, EXT_SDA)

    // USB
    DEFINE_PIN(GPIO_NUM_12, DMINUS, "D-")
    DEFINE_PIN(GPIO_NUM_13, DPLUS, "D+")

    // Flow meter B
    DEFINE_PIN(GPIO_NUM_15, IFLOWB)

    // UART
    DEFINE_PIN(GPIO_NUM_16, TXD0)
    DEFINE_PIN(GPIO_NUM_17, RXD0)

    // Motor control pins
    DEFINE_PIN(GPIO_NUM_18, DAIN2)
    DEFINE_PIN(GPIO_NUM_19, DAIN1)
    DEFINE_PIN(GPIO_NUM_20, DBIN2)
    DEFINE_PIN(GPIO_NUM_21, DBIN1)

    // Enable / disable external load
    DEFINE_PIN(GPIO_NUM_22, LOADEN)

    // Motor fault pin
    DEFINE_PIN(GPIO_NUM_23, NFAULT)
};

}    // namespace cornucopia::ugly_duckling::devices
