#pragma once

#include <Log.hpp>
#include <Pin.hpp>
#include <PwmManager.hpp>
#include <drivers/MotorDriver.hpp>
#include <drivers/SharedEnable.hpp>

#include <memory>

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
    Drv8801Driver(
        const std::shared_ptr<PwmManager>& pwm,
        const PinPtr& enablePin,
        const InternalPinPtr& phasePin,
        const PinPtr& mode1Pin,
        const PinPtr& mode2Pin,
        const PinPtr& currentPin,
        const PinPtr& faultPin,
        const std::shared_ptr<SharedEnable>& enable)
        : enablePin(enablePin)
        , phaseChannel(pwm->registerPin(phasePin, PWM_FREQ, PWM_RESOLUTION))
        , currentPin(currentPin)
        , faultPin(faultPin)
        , enableHandle(enable->createHandle()) {

        LOGI("Initializing DRV8801 on pins enable = %s, phase = %s, fault = %s, mode1 = %s, mode2 = %s, current = %s",
            enablePin->getName().c_str(),
            phasePin->getName().c_str(),
            faultPin->getName().c_str(),
            mode1Pin->getName().c_str(),
            mode2Pin->getName().c_str(),
            currentPin->getName().c_str());

        enablePin->pinMode(Pin::Mode::Output);
        mode1Pin->pinMode(Pin::Mode::Output);
        mode2Pin->pinMode(Pin::Mode::Output);
        faultPin->pinMode(Pin::Mode::Input);
        currentPin->pinMode(Pin::Mode::Input);

        // TODO Allow using the DRV8801 in other modes
        mode1Pin->digitalWrite(1);
        mode2Pin->digitalWrite(1);
        // Sleep pin is managed by SharedEnable — starts in inactive (sleeping) state
    }

    void drive(MotorPhase phase, double duty) override {
        if (duty == 0) {
            LOGD("Stopping");
            enableHandle.release();
            enablePin->digitalWrite(0);
            return;
        }
        enableHandle.acquire();
        enablePin->digitalWrite(1);

        int direction = (phase == MotorPhase::Forward ? 1 : -1);
        int dutyValue = static_cast<int>(phaseChannel.maxValue() * (0.5 + direction * duty / 2));
        LOGD("Driving motor %s at %.2f%%",
            phase == MotorPhase::Forward ? "forward" : "reverse",
            duty * 100);

        phaseChannel.write(dutyValue);
    }

    void brake() override {
        LOGD("Braking");
        enableHandle.acquire();
        // ENABLE = 0 with MODE1 = 1 while awake: brake (slow decay)
        enablePin->digitalWrite(0);
    }

private:
    const PinPtr enablePin;
    const PwmPin& phaseChannel;
    const PinPtr currentPin;
    const PinPtr faultPin;
    SharedEnable::Handle enableHandle;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
