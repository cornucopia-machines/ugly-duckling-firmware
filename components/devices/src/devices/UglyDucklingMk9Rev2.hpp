#pragma once

#include <memory>

#include <MacAddress.hpp>
#include <Pin.hpp>
#include <drivers/Bq27220Driver.hpp>
#include <drivers/Drv8848Driver.hpp>
#include <drivers/Ina219Driver.hpp>
#include <drivers/LedDriver.hpp>

#include <peripherals/Peripheral.hpp>
#include <peripherals/door/Door.hpp>
#include <peripherals/valve/ValveFactory.hpp>

#include <devices/DeviceDefinition.hpp>

using namespace farmhub::kernel;
using namespace farmhub::kernel::drivers;
using namespace farmhub::peripherals::door;
using namespace farmhub::peripherals::valve;

namespace farmhub::devices {

class UglyDucklingMk9Rev2 : public DeviceDefinition {
public:
    explicit UglyDucklingMk9Rev2()
        : DeviceDefinition({ .model = "mk9", .revision = 2, .boot = GPIO_NUM_9, .status = GPIO_NUM_8 }) {
    }

    std::shared_ptr<BatteryDriver> createBatteryDriver(const std::shared_ptr<I2CManager>& i2c) override {
        return std::make_shared<Bq27220Driver>(
            i2c,
            SDA,
            SCL,
            BatteryParameters {
                .maximumVoltage = 4100,
                .bootThreshold = 3500,
                .shutdownThreshold = 3300,
            });
    }

    std::shared_ptr<Ina219Driver> ina219;

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

        ina219 = std::make_shared<Ina219Driver>(
            services.i2c,
            I2CConfig {
                .address = Ina219Driver::DEFAULT_ADDRESS,
                .sda = SDA,
                .scl = SCL,
            },
            Ina219Parameters {
                .uRange = INA219_BUS_RANGE_16V,
                .gain = INA219_GAIN_0_125,
                .uResolution = INA219_RES_12BIT_1S,
                .iResolution = INA219_RES_12BIT_1S,
                .mode = INA219_MODE_CONT_SHUNT_BUS,
                .shuntMilliOhm = 50,
            });
    }

    // Battery fuel gauge interrupt
    DEFINE_PIN(GPIO_NUM_0, BAT_GAUGE)

    // Flow meter A
    DEFINE_PIN(GPIO_NUM_1, IFLOWA)

    // Legacy soil moisture sensor pin
    DEFINE_PIN(GPIO_NUM_3, ISOILML)

    // Motor fault pin
    DEFINE_PIN(GPIO_NUM_4, NFAULT)

    // Enable / disable external load
    DEFINE_PIN(GPIO_NUM_5, LOADEN)

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

    // Legacy soil temperature sensor
    DEFINE_PIN(GPIO_NUM_22, ISOILTL)

    // Enable pin for legacy soil moisture sensor
    DEFINE_PIN(GPIO_NUM_23, NENSOILM)
};

}    // namespace farmhub::devices
