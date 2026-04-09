#pragma once

#include <atomic>
#include <chrono>
#include <memory>

#include <Log.hpp>
#include <Pin.hpp>
#include <PwmManager.hpp>
#include <drivers/MotorDriver.hpp>

using namespace std::chrono;

namespace cornucopia::ugly_duckling::kernel::drivers {

/**
 * @brief Texas Instruments DRV8801 motor driver.
 *
 * https://www.ti.com/lit/gpn/DRV8801
 */
class Drv8801Driver
    : public PwmMotorDriver {

private:
    static constexpr uint32_t PWM_FREQ = 25000;
    static constexpr ledc_timer_bit_t PWM_RESOLUTION = LEDC_TIMER_8_BIT;

public:
    // Note: on Ugly Duckling MK5, the DRV8874's PMODE is wired to 3.3V, so it's locked in PWM mode
    Drv8801Driver(
        const std::shared_ptr<PwmManager>& pwm,
        const PinPtr& enablePin,
        const InternalPinPtr& phasePin,
        const PinPtr& mode1Pin,
        const PinPtr& mode2Pin,
        const PinPtr& currentPin,
        const PinPtr& faultPin,
        const PinPtr& sleepPin)
        : enablePin(enablePin)
        , phaseChannel(pwm->registerPin(phasePin, PWM_FREQ, PWM_RESOLUTION))
        , currentPin(currentPin)
        , faultPin(faultPin)
        , sleepPin(sleepPin) {

        LOGI("Initializing DRV8801 on pins enable = %s, phase = %s, fault = %s, sleep = %s, mode1 = %s, mode2 = %s, current = %s",
            enablePin->getName().c_str(),
            phasePin->getName().c_str(),
            faultPin->getName().c_str(),
            sleepPin->getName().c_str(),
            mode1Pin->getName().c_str(),
            mode2Pin->getName().c_str(),
            currentPin->getName().c_str());

        enablePin->pinMode(Pin::Mode::Output);
        mode1Pin->pinMode(Pin::Mode::Output);
        mode2Pin->pinMode(Pin::Mode::Output);
        sleepPin->pinMode(Pin::Mode::Output);
        faultPin->pinMode(Pin::Mode::Input);
        currentPin->pinMode(Pin::Mode::Input);

        // TODO Allow using the DRV8801 in other modes
        mode1Pin->digitalWrite(1);
        mode2Pin->digitalWrite(1);

        sleep();
    }

    void drive(MotorPhase phase, double duty) override {
        if (duty == 0) {
            LOGD("Stopping");
            sleep();
            enablePin->digitalWrite(0);
            return;
        }
        wakeUp();
        enablePin->digitalWrite(1);

        int direction = (phase == MotorPhase::Forward ? 1 : -1);
        int dutyValue = static_cast<int>(phaseChannel.maxValue() * (0.5 + direction * duty / 2));
        LOGD("Driving motor %s at %.2f%%",
            phase == MotorPhase::Forward ? "forward" : "reverse",
            duty * 100);

        phaseChannel.write(dutyValue);
    }

    void sleep() {
        sleepPin->digitalWrite(0);
        sleeping = true;
    }

    void wakeUp() {
        sleepPin->digitalWrite(1);
        sleeping = false;
    }

    bool isSleeping() const {
        return sleeping;
    }

private:
    const PinPtr enablePin;
    const PwmPin& phaseChannel;
    const PinPtr currentPin;
    const PinPtr faultPin;
    const PinPtr sleepPin;

    std::atomic<bool> sleeping { false };
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
