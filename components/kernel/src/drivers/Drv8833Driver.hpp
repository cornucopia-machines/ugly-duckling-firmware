#pragma once

#include <EspException.hpp>
#include <Log.hpp>
#include <Pin.hpp>
#include <PwmManager.hpp>
#include <Task.hpp>
#include <drivers/MotorDriver.hpp>
#include <drivers/PulseTimer.hpp>
#include <drivers/SharedEnable.hpp>

#include <chrono>
#include <memory>

namespace cornucopia::ugly_duckling::kernel::drivers {

/**
 * @brief Texas Instruments DRV8833 dual motor driver.
 *
 * https://www.ti.com/lit/gpn/DRV8833
 */
class Drv8833Driver {

public:
    static std::shared_ptr<Drv8833Driver> create(
        const std::shared_ptr<PwmManager>& pwm,
        const InternalPinPtr& ain1Pin,
        const InternalPinPtr& ain2Pin,
        const InternalPinPtr& bin1Pin,
        const InternalPinPtr& bin2Pin,
        const PinPtr& faultPin,
        const std::shared_ptr<SharedEnable>& enable,
        bool reverse = false) {
        return std::make_shared<Drv8833Driver>(pwm, ain1Pin, ain2Pin, bin1Pin, bin2Pin, faultPin, enable, reverse);
    }

    std::shared_ptr<PwmMotorDriver> getMotorA() {
        return motorA;
    }

    std::shared_ptr<PwmMotorDriver> getMotorB() {
        return motorB;
    }

    Drv8833Driver(
        const std::shared_ptr<PwmManager>& pwm,
        const InternalPinPtr& ain1Pin,
        const InternalPinPtr& ain2Pin,
        const InternalPinPtr& bin1Pin,
        const InternalPinPtr& bin2Pin,
        const PinPtr& faultPin,
        const std::shared_ptr<SharedEnable>& enable,
        bool reverse = false)
        : faultPin(faultPin) {

        LOGI("Initializing motor driver on pin fault = %s",
            faultPin->getName().c_str());

        faultPin->pinMode(Pin::Mode::Input);

        LOGI("Initializing motors on pins ain1 = %s, ain2 = %s, bin1 = %s, bin2 = %s",
            ain1Pin->getName().c_str(),
            ain2Pin->getName().c_str(),
            bin1Pin->getName().c_str(),
            bin2Pin->getName().c_str());
        motorA = std::make_shared<Drv8833MotorDriver>(pwm, ain1Pin, ain2Pin, enable, reverse);
        motorB = std::make_shared<Drv8833MotorDriver>(pwm, bin1Pin, bin2Pin, enable, reverse);
    }

private:
    class Drv8833MotorDriver : public PwmMotorDriver {
    private:
        static constexpr uint32_t PWM_FREQ = 25000;
        static constexpr ledc_timer_bit_t PWM_RESOLUTION = LEDC_TIMER_10_BIT;

    public:
        Drv8833MotorDriver(
            const std::shared_ptr<PwmManager>& pwm,
            const InternalPinPtr& in1Pin,
            const InternalPinPtr& in2Pin,
            const std::shared_ptr<SharedEnable>& enable,
            bool reverse)
            : forwardChannel(pwm->registerPin(reverse ? in1Pin : in2Pin, PWM_FREQ, PWM_RESOLUTION))
            , reverseChannel(pwm->registerPin(reverse ? in2Pin : in1Pin, PWM_FREQ, PWM_RESOLUTION))
            , enableHandle(enable->createHandle()) {
        }

        void drive(MotorPhase phase, double duty) override {
            if (duty == 0) {
                LOGD("Stopping motor on pins %s/%s",
                    forwardChannel.getName().c_str(),
                    reverseChannel.getName().c_str());
                forwardChannel.write(0);
                reverseChannel.write(0);
                enableHandle.release();
                return;
            }

            enableRail();

            uint32_t dutyValue = toDutyValue(duty);
            LOGD("Driving motor %s on pins %s/%s at %d%%",
                phase == MotorPhase::Forward ? "forward" : "reverse",
                forwardChannel.getName().c_str(),
                reverseChannel.getName().c_str(),
                (int) (duty * 100));

            ESP_ERROR_THROW(writeDrive(phase, dutyValue));
        }

        void brake() override {
            LOGD("Braking motor on pins %s/%s",
                forwardChannel.getName().c_str(),
                reverseChannel.getName().c_str());
            enableRail();
            // xIN1 = xIN2 = 1: both low-side FETs on, slow decay
            forwardChannel.write(forwardChannel.maxValue());
            reverseChannel.write(reverseChannel.maxValue());
        }

        void drivePulse(MotorPhase phase, double duty, std::chrono::milliseconds duration) override {
            enableRail();

            uint32_t dutyValue = toDutyValue(duty);
            LOGD("Pulsing motor %s on pins %s/%s at %d%% for %lld ms",
                phase == MotorPhase::Forward ? "forward" : "reverse",
                forwardChannel.getName().c_str(),
                reverseChannel.getName().c_str(),
                (int) (duty * 100),
                duration.count());

            // Only hold on to a GPTimer for the duration of the pulse: there are only a few of them (two on the ESP32-C6)
            PulseTimer pulseTimer(forwardChannel, reverseChannel);
            // The timer's alarm brakes the bridge (both inputs held high) exactly `duration` after the drive starts
            esp_err_t writeResult = ESP_OK;
            pulseTimer.run(duration, [&]() {
                writeResult = writeDrive(phase, dutyValue);
            });
            ESP_ERROR_THROW(writeResult);
        }

    private:
        uint32_t toDutyValue(double duty) const {
            return static_cast<uint32_t>((forwardChannel.maxValue() + forwardChannel.maxValue() * duty) / 2);
        }

        /**
         * @brief Set the inputs to drive in `phase`; doesn't throw, so it can run in a critical section.
         */
        esp_err_t writeDrive(MotorPhase phase, uint32_t dutyValue) const noexcept {
            const PwmPin& active = phase == MotorPhase::Forward ? forwardChannel : reverseChannel;
            const PwmPin& inactive = phase == MotorPhase::Forward ? reverseChannel : forwardChannel;
            esp_err_t err = active.tryWrite(dutyValue);
            if (err != ESP_OK) {
                return err;
            }
            return inactive.tryWrite(0);
        }

        /**
         * @brief Time for the load rail to come up after the enable is asserted.
         *
         * @details On boards where the enable also switches a load boost converter (MK14's
         * TPS61378-Q1 has a true load disconnect, so VLOAD starts from 0 V with ~0.4 ms delay
         * plus 2.5 ms soft-start), loading the rail during soft-start can trip the boost's hiccup
         * protection. It also covers the DRV88xx wake-up time. Harmless on boards without a boost.
         */
        static constexpr std::chrono::milliseconds RAIL_SETTLE_TIME { 5 };

        /**
         * @brief Acquire the enable before touching the inputs, and wait for the rail if we just turned it on.
         */
        void enableRail() {
            if (!enableHandle.isAcquired()) {
                enableHandle.acquire();
                Task::delay(RAIL_SETTLE_TIME);
            }
        }

        const PwmPin& forwardChannel;
        const PwmPin& reverseChannel;
        SharedEnable::Handle enableHandle;
    };

    std::shared_ptr<Drv8833MotorDriver> motorA;
    std::shared_ptr<Drv8833MotorDriver> motorB;
    const PinPtr faultPin;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
