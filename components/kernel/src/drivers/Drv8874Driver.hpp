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
 * @brief Texas Instruments DRV8874 motor driver.
 *
 * https://www.ti.com/lit/gpn/DRV8874
 */
class Drv8874Driver
    : public PwmMotorDriver {

private:
    static constexpr uint32_t PWM_FREQ = 25000;
    static constexpr ledc_timer_bit_t PWM_RESOLUTION = LEDC_TIMER_8_BIT;

public:
    // Note: on Ugly Duckling MK5, the DRV8874's PMODE is wired to 3.3V, so it's locked in PWM mode
    Drv8874Driver(
        const std::shared_ptr<PwmManager>& pwm,
        const InternalPinPtr& in1Pin,
        const InternalPinPtr& in2Pin,
        const PinPtr& currentPin,
        const PinPtr& faultPin,
        const PinPtr& sleepPin)
        : in1Channel(pwm->registerPin(in1Pin, PWM_FREQ, PWM_RESOLUTION))
        , in2Channel(pwm->registerPin(in2Pin, PWM_FREQ, PWM_RESOLUTION))
        , currentPin(currentPin)
        , faultPin(faultPin)
        , sleepPin(sleepPin) {

        LOGI("Initializing DRV8874 on pins in1 = %s, in2 = %s, fault = %s, sleep = %s, current = %s",
            in1Pin->getName().c_str(),
            in2Pin->getName().c_str(),
            faultPin->getName().c_str(),
            sleepPin->getName().c_str(),
            currentPin->getName().c_str());

        sleepPin->pinMode(Pin::Mode::Output);
        faultPin->pinMode(Pin::Mode::Input);
        currentPin->pinMode(Pin::Mode::Input);

        sleep();
    }

    void drive(MotorPhase phase, double duty) override {
        if (duty == 0) {
            LOGD("Stopping motor");
            sleep();
            return;
        }
        wakeUp();

        int dutyValue = static_cast<int>((in1Channel.maxValue() + in1Channel.maxValue() * duty) / 2);
        LOGD("Driving motor %s at %.2f%%",
            phase == MotorPhase::Forward ? "forward" : "reverse",
            duty * 100);

        switch (phase) {
            case MotorPhase::Forward:
                in1Channel.write(dutyValue);
                in2Channel.write(0);
                break;
            case MotorPhase::Reverse:
                in1Channel.write(0);
                in2Channel.write(dutyValue);
                break;
        }
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
    const PwmPin& in1Channel;
    const PwmPin& in2Channel;
    const PinPtr currentPin;
    const PinPtr faultPin;
    const PinPtr sleepPin;

    std::atomic<bool> sleeping { false };
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
