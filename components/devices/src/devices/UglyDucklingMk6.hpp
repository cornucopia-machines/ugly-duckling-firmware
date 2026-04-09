#pragma once

#include <map>
#include <memory>

#include <Pin.hpp>
#include <drivers/BatteryDriver.hpp>
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

class UglyDucklingMk6Base : public DeviceDefinition {
public:
    explicit UglyDucklingMk6Base(int revision)
        : DeviceDefinition({ .model = "mk6", .revision = revision, .boot = GPIO_NUM_0, .status = GPIO_NUM_2 }) {
        // Switch off strapping pin
        // TODO(lptr): Add a LED driver instead
        LEDA_RED->pinMode(Pin::Mode::Output);
        LEDA_RED->digitalWrite(1);
    }

    std::shared_ptr<BatteryDriver> createBatteryDriver(const std::shared_ptr<I2CManager>& _i2c) override {
        return std::make_shared<AnalogBatteryDriver>(
            BATTERY,
            1.2424,
            BatteryParameters {
                .maximumVoltage = 4100,
                .bootThreshold = 3600,
                .shutdownThreshold = 3400,
            });
    }

protected:
    virtual PinPtr motorNSleepPin() const = 0;

    void registerDeviceSpecificPeripheralFactories(const std::shared_ptr<PeripheralManager>& peripheralManager, const PeripheralServices& services, const std::shared_ptr<DeviceSettings>& settings) override {
        auto nSleepPin = settings->motorNSleepPin.getOrDefault(motorNSleepPin());
        auto motorDriver = Drv8833Driver::create(
            services.pwmManager,
            AIN1,
            AIN2,
            BIN1,
            BIN2,
            NFault,
            nSleepPin,
            true);

        std::map<std::string, std::shared_ptr<PwmMotorDriver>> motors = { { "a", motorDriver->getMotorA() }, { "b", motorDriver->getMotorB() } };

        peripheralManager->registerFactory(valve::makeFactory(motors, ValveControlStrategyType::Latching));
        peripheralManager->registerFactory(door::makeFactory(motors));
    }

protected:
    DEFINE_PIN(GPIO_NUM_1, BATTERY)
    DEFINE_PIN(GPIO_NUM_4, STATUS2)
    DEFINE_PIN(GPIO_NUM_5, IOB1, "B1")
    DEFINE_PIN(GPIO_NUM_6, IOA1, "A1")
    DEFINE_PIN(GPIO_NUM_7, DIPROPI)
    DEFINE_PIN(GPIO_NUM_15, IOA2, "A2")
    DEFINE_PIN(GPIO_NUM_16, AIN1)
    DEFINE_PIN(GPIO_NUM_17, AIN2)
    DEFINE_PIN(GPIO_NUM_18, BIN2)
    DEFINE_PIN(GPIO_NUM_8, BIN1)
    DEFINE_PIN(GPIO_NUM_19, DMINUS, "D-")
    DEFINE_PIN(GPIO_NUM_20, DPLUS, "D+")
    DEFINE_PIN(GPIO_NUM_46, LEDA_RED)
    DEFINE_PIN(GPIO_NUM_9, LEDA_GREEN)
    DEFINE_PIN(GPIO_NUM_11, NFault)
    DEFINE_PIN(GPIO_NUM_12, BTN1)
    DEFINE_PIN(GPIO_NUM_13, BTN2)
    DEFINE_PIN(GPIO_NUM_14, IOC4, "C4")
    DEFINE_PIN(GPIO_NUM_21, IOC3, "C3")
    DEFINE_PIN(GPIO_NUM_47, IOC2, "C2")
    DEFINE_PIN(GPIO_NUM_48, IOC1, "C1")
    DEFINE_PIN(GPIO_NUM_45, IOB2, "B2")
    DEFINE_PIN(GPIO_NUM_35, SDA)
    DEFINE_PIN(GPIO_NUM_36, SCL)
    DEFINE_PIN(GPIO_NUM_37, LEDB_GREEN)
    DEFINE_PIN(GPIO_NUM_38, LEDB_RED)
    DEFINE_PIN(GPIO_NUM_39, TCK)
    DEFINE_PIN(GPIO_NUM_40, TDO)
    DEFINE_PIN(GPIO_NUM_41, TDI)
    DEFINE_PIN(GPIO_NUM_42, TMS)
    DEFINE_PIN(GPIO_NUM_44, RXD0)
    DEFINE_PIN(GPIO_NUM_43, TXD0)
    // Available on MK6 Rev3+
    DEFINE_PIN(GPIO_NUM_10, LOADEN)
};

// MAC prefix 0x34:0x85:0x18
class UglyDucklingMk6Rev1 : public UglyDucklingMk6Base {
public:
    UglyDucklingMk6Rev1() : UglyDucklingMk6Base(1) {}

protected:
    PinPtr motorNSleepPin() const override {
        return IOC2;
    }
};

// MAC prefix 0xec:0xda:0x3b:0x5b
class UglyDucklingMk6Rev2 : public UglyDucklingMk6Base {
public:
    UglyDucklingMk6Rev2() : UglyDucklingMk6Base(2) {}

protected:
    PinPtr motorNSleepPin() const override {
        return IOC2;
    }
};

// All other known MK6 MAC ranges
class UglyDucklingMk6Rev3 : public UglyDucklingMk6Base {
public:
    UglyDucklingMk6Rev3() : UglyDucklingMk6Base(3) {}

protected:
    PinPtr motorNSleepPin() const override {
        return LOADEN;
    }
};

}    // namespace cornucopia::ugly_duckling::devices
