#pragma once

#include <Pin.hpp>
#include <drivers/Bq27220Driver.hpp>
#include <drivers/Drv8833Driver.hpp>
#include <drivers/LedDriver.hpp>

#include <peripherals/Peripheral.hpp>
#include <peripherals/door/Door.hpp>
#include <peripherals/valve/ValveFactory.hpp>

#include <devices/DeviceDefinition.hpp>

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::peripherals::door;
using namespace cornucopia::ugly_duckling::peripherals::valve;

namespace cornucopia::ugly_duckling::devices {

class UglyDucklingMk7 : public DeviceDefinition {
public:
    UglyDucklingMk7()
        : DeviceDefinition({ .model = "mk7", .revision = 1, .boot = GPIO_NUM_0, .status = GPIO_NUM_15 }) {
        // Switch off strapping pin
        // TODO: Add a LED driver instead
        STATUS2->pinMode(Pin::Mode::Output);
        STATUS2->digitalWrite(1);
    }

    std::shared_ptr<BatteryDriver> createBatteryDriver(const std::shared_ptr<I2CManager>& i2c) override {
        return std::make_shared<Bq27220Driver>(
            i2c,
            SDA,
            SCL,
            BatteryParameters {
                .maximumVoltage = 4100,
                .bootThreshold = 3600,
                .shutdownThreshold = 3000,
            });
    }

protected:
    void registerDeviceSpecificPeripheralFactories(const std::shared_ptr<PeripheralManager>& peripheralManager, const PeripheralServices& services, const std::shared_ptr<DeviceSettings>& _settings) override {
        auto motorDriver = Drv8833Driver::create(
            services.pwmManager,
            DAIN1,
            DAIN2,
            DBIN1,
            DBIN2,
            DNFault,
            LOADEN);

        std::map<std::string, std::shared_ptr<PwmMotorDriver>> motors = { { "a", motorDriver->getMotorA() }, { "b", motorDriver->getMotorB() } };

        peripheralManager->registerFactory(valve::makeFactory(motors, ValveControlStrategyType::Latching));
        peripheralManager->registerFactory(door::makeFactory(motors));
    }

private:
    DEFINE_PIN(GPIO_NUM_1, IOA2, "A2")
    DEFINE_PIN(GPIO_NUM_2, IOA1, "A1")
    DEFINE_PIN(GPIO_NUM_3, IOA3, "A3")
    DEFINE_PIN(GPIO_NUM_4, IOB3, "B3")
    DEFINE_PIN(GPIO_NUM_5, IOB1, "B1")
    DEFINE_PIN(GPIO_NUM_6, IOB2, "B2")
    DEFINE_PIN(GPIO_NUM_8, BAT_GPIO)
    DEFINE_PIN(GPIO_NUM_9, FSPIHD)
    DEFINE_PIN(GPIO_NUM_10, FSPICS0)
    DEFINE_PIN(GPIO_NUM_11, FSPID)
    DEFINE_PIN(GPIO_NUM_12, FSPICLK)
    DEFINE_PIN(GPIO_NUM_13, FSPIQ)
    DEFINE_PIN(GPIO_NUM_14, FSPIWP)
    DEFINE_PIN(GPIO_NUM_16, LOADEN)
    DEFINE_PIN(GPIO_NUM_17, SCL)
    DEFINE_PIN(GPIO_NUM_18, SDA)
    DEFINE_PIN(GPIO_NUM_19, DMINUS, "D-")
    DEFINE_PIN(GPIO_NUM_20, DPLUS, "D+")
    DEFINE_PIN(GPIO_NUM_21, IOX1, "X1")
    DEFINE_PIN(GPIO_NUM_37, DBIN1)
    DEFINE_PIN(GPIO_NUM_38, DBIN2)
    DEFINE_PIN(GPIO_NUM_39, DAIN2)
    DEFINE_PIN(GPIO_NUM_40, DAIN1)
    DEFINE_PIN(GPIO_NUM_41, DNFault)
    DEFINE_PIN(GPIO_NUM_43, TXD0)
    DEFINE_PIN(GPIO_NUM_44, RXD0)
    DEFINE_PIN(GPIO_NUM_45, IOX2, "X2")
    DEFINE_PIN(GPIO_NUM_46, STATUS2)
    DEFINE_PIN(GPIO_NUM_47, IOB4, "B4")
    DEFINE_PIN(GPIO_NUM_48, IOA4, "A4")
};

}    // namespace cornucopia::ugly_duckling::devices
