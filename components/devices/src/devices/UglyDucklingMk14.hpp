#pragma once
#include <MacAddress.hpp>
#include <Pin.hpp>
#include <devices/DeviceDefinition.hpp>
#include <drivers/Bq27220Driver.hpp>
#include <drivers/BuzzerDriver.hpp>
#include <drivers/Drv8847Driver.hpp>
#include <drivers/LedDriver.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/door/Door.hpp>
#include <peripherals/valve/ValveFactory.hpp>

#include <soc/rtc.h>

#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::drivers;
using namespace cornucopia::ugly_duckling::peripherals::door;
using namespace cornucopia::ugly_duckling::peripherals::environment;
using namespace cornucopia::ugly_duckling::peripherals::valve;

namespace cornucopia::ugly_duckling::devices {

class UglyDucklingMk14Rev1 : public DeviceDefinition {
public:
    explicit UglyDucklingMk14Rev1()
        : DeviceDefinition({ .model = "mk14", .revision = 1, .boot = GPIO_NUM_9, .status = GPIO_NUM_8 }) {
        rtc_clk_32k_enable(true);

        // Pull buzzer low so it stays quiet until BuzzerDriver claims the pin.
        // Parked rather than configured via pinMode() so LEDC doesn't report a
        // conflict when the buzzer driver registers the pin later on.
        BUZZER->parkOutput(0);
    }

    Result<std::shared_ptr<BatteryDriver>> createBatteryDriver(const std::shared_ptr<I2CManager>& i2c) override {
        auto driver = TRY(Bq27220Driver::create(i2c, LP_SDA, LP_SCL, BatteryParameters {
                                                                         .maximumVoltage = 4100,
                                                                         .bootThreshold = 3600,
                                                                         .shutdownThreshold = 3500,
                                                                     }));
        driver->useExternalThermistor();
        return driver;
    }

    void handleShortButtonPress(milliseconds duration) override {
        if (buzzer && duration >= 200ms) {
            buzzer->buzz(1s, 0.1);
        }
    }

    std::optional<cellular::CellularModemPins> getCellularModemPins() const override {
        // JMODEM1 takes Desert Lark MK2 (nRF9151). It's wired straight (the MK13 TX/RX swap is
        // fixed): GPIO16 (U0TXD) drives the modem's RX, GPIO17 (U0RXD) listens to its TX
        return cellular::CellularModemPins {
            .tx = TXD0,
            .rx = RXD0,
            .rts = MDM_CTS,
            .cts = MDM_RTS,
            .dtr = MDM_DTR,
            .ri = MDM_RI,
            .reset = MDM_RESET,
        };
    }

    std::shared_ptr<BuzzerDriver> buzzer;

protected:
    void registerDeviceSpecificPeripheralFactories(const std::shared_ptr<PeripheralManager>& peripheralManager, const PeripheralServices& services, const std::shared_ptr<DeviceConfiguration>& /*deviceConfig*/) override {
        // LOADEN switches the 6.5 V VLOAD boost, the DRV8847's nSLEEP and the +3V3_BUS load switch
        // (RS485 transceiver and I2C port) together. 5 ms covers the boost's soft start and the
        // DRV8847's 1.5 ms wake time.
        auto loadEnable = SharedEnable::forActiveHighPin({ .pin = LOADEN, .settleTime = 5ms });

        // Only bridge A is wired (to JDRIVE1); IN3/IN4 are tied to GND on the board
        auto motorDriver = Drv8847Driver::create(
            services.pwmManager,
            AIN1,
            AIN2,
            nullptr,
            nullptr,
            NFAULT,
            loadEnable);

        buzzer = std::make_shared<BuzzerDriver>(services.pwmManager, BUZZER, loadEnable);

        std::map<std::string, std::shared_ptr<PwmMotorDriver>> motors = { { "a", motorDriver->getMotorA() } };

        peripheralManager->registerFactory(valve::makeFactory(motors, ValveControlStrategyType::Latching));
        peripheralManager->registerFactory(door::makeFactory(motors));
    }

    // 32 kHz external crystal
    DEFINE_PIN(GPIO_NUM_0, XTAL32K_P, "XTAL_32K_P")
    DEFINE_PIN(GPIO_NUM_1, XTAL32K_N, "XTAL_32K_N")

    // Motor fault pin
    DEFINE_PIN(GPIO_NUM_2, NFAULT)

    // Cellular modem ring indicator (push-pull from the modem, floats with no modem fitted);
    // LP GPIO, so it can wake the chip from deep sleep
    DEFINE_PIN(GPIO_NUM_3, MDM_RI, "MDM_RI#")

    // Cellular modem reset: open-drain only, never drive it high or enable the pull-up
    // (the nRF9151's nRESET idles at ~2.2 V from its internal pull-up)
    DEFINE_PIN(GPIO_NUM_4, MDM_RESET, "MDM_RESET#")

    // Flow meter (MK13 and earlier had a second one on GPIO3)
    DEFINE_PIN(GPIO_NUM_5, IFLOWA)

    // I2C: on-board fuel gauge, plus the JI2C1 port behind an isolator that only
    // connects while LOADEN is high (there is no separate external bus since MK14)
    DEFINE_PIN(GPIO_NUM_6, LP_SDA)
    DEFINE_PIN(GPIO_NUM_7, LP_SCL)

    // Status LED 2
    DEFINE_PIN(GPIO_NUM_9, STATUS2)

    // RS485 sensor ports through an auto-direction THVD1406 (powered while LOADEN is high)
    DEFINE_PIN(GPIO_NUM_10, RS485_TX)
    DEFINE_PIN(GPIO_NUM_11, RS485_RX)

    // USB
    DEFINE_PIN(GPIO_NUM_12, DMINUS, "D-")
    DEFINE_PIN(GPIO_NUM_13, DPLUS, "D+")

    // Cellular modem DTR (high = deasserted, modem UART off; 100k pull-up on the board)
    DEFINE_PIN(GPIO_NUM_15, MDM_DTR, "MDM_DTR#")

    // UART0: shared between the pogo-pin factory header (JPOGO1)
    // and the cellular modem connector (JMODEM1). The console is on USB Serial/JTAG so the
    // modem can have them; UD_UART0_CONSOLE builds move the console back here instead.
    DEFINE_PIN(GPIO_NUM_16, TXD0)
    DEFINE_PIN(GPIO_NUM_17, RXD0)

    // Motor control pins (bridge A only)
    DEFINE_PIN(GPIO_NUM_18, AIN2)
    DEFINE_PIN(GPIO_NUM_19, AIN1)

    // Cellular modem hardware flow control, named after the modem's signals like on the schematic:
    // GPIO20 is the MCU's CTS input (modem RTS), GPIO21 the MCU's RTS output (modem CTS)
    DEFINE_PIN(GPIO_NUM_20, MDM_RTS)
    DEFINE_PIN(GPIO_NUM_21, MDM_CTS)

    // Enable / disable external load
    DEFINE_PIN(GPIO_NUM_22, LOADEN)

    // Buzzer
    DEFINE_PIN(GPIO_NUM_23, BUZZER)
};

}    // namespace cornucopia::ugly_duckling::devices
