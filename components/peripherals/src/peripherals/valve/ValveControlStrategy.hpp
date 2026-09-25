#pragma once
#include <Strings.hpp>
#include <Task.hpp>
#include <drivers/MotorDriver.hpp>
#include <peripherals/api/IValve.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

using namespace std::chrono;
using namespace cornucopia::ugly_duckling::kernel::drivers;
using namespace cornucopia::ugly_duckling::peripherals::api;

namespace cornucopia::ugly_duckling::peripherals::valve {

enum class ValveControlStrategyType : uint8_t {
    NormallyOpen,
    NormallyClosed,
    Latching
};

class ValveControlStrategy {
public:
    virtual ~ValveControlStrategy() = default;

    virtual void open() = 0;
    virtual void close() = 0;
    virtual TargetState getDefaultState() const = 0;

    virtual std::string describe() const = 0;
};

class MotorValveControlStrategy
    : public ValveControlStrategy {
public:
    MotorValveControlStrategy(const std::shared_ptr<PwmMotorDriver>& controller)
        : controller(controller) {
    }

protected:
    const std::shared_ptr<PwmMotorDriver> controller;
};

class HoldingMotorValveControlStrategy
    : public MotorValveControlStrategy {

public:
    HoldingMotorValveControlStrategy(const std::shared_ptr<PwmMotorDriver>& controller, milliseconds switchDuration, double holdDuty)
        : MotorValveControlStrategy(controller)
        , switchDuration(switchDuration)
        , holdDuty(holdDuty) {
    }

protected:
    void driveAndHold(TargetState targetState) {
        switch (targetState) {
            case TargetState::Open:
                driveAndHold(MotorPhase::Forward);
                break;
            case TargetState::Closed:
                driveAndHold(MotorPhase::Reverse);
                break;
            default:
                // Ignore
                break;
        }
    }

    const milliseconds switchDuration;
    const double holdDuty;

private:
    void driveAndHold(MotorPhase phase) {
        controller->drive(phase, 1.0);
        Task::delay(switchDuration);
        controller->drive(phase, holdDuty);
    }
};

class NormallyClosedMotorValveControlStrategy
    : public HoldingMotorValveControlStrategy {
public:
    NormallyClosedMotorValveControlStrategy(const std::shared_ptr<PwmMotorDriver>& controller, milliseconds switchDuration, double holdDuty)
        : HoldingMotorValveControlStrategy(controller, switchDuration, holdDuty) {
    }

    void open() override {
        driveAndHold(TargetState::Open);
    }

    void close() override {
        controller->stop();
    }

    TargetState getDefaultState() const override {
        return TargetState::Closed;
    }

    std::string describe() const override {
        return "normally closed with switch duration " + std::to_string(switchDuration.count()) + " ms and hold duty " + kernel::toStringWithPrecision(holdDuty * 100, 1) + "%";
    }
};

class NormallyOpenMotorValveControlStrategy
    : public HoldingMotorValveControlStrategy {
public:
    NormallyOpenMotorValveControlStrategy(const std::shared_ptr<PwmMotorDriver>& controller, milliseconds switchDuration, double holdDuty)
        : HoldingMotorValveControlStrategy(controller, switchDuration, holdDuty) {
    }

    void open() override {
        controller->stop();
    }

    void close() override {
        driveAndHold(TargetState::Closed);
    }

    TargetState getDefaultState() const override {
        return TargetState::Open;
    }

    std::string describe() const override {
        return "normally open with switch duration " + std::to_string(switchDuration.count()) + " ms and hold duty " + kernel::toStringWithPrecision(holdDuty * 100, 1) + "%";
    }
};

class LatchingMotorValveControlStrategy
    : public MotorValveControlStrategy {
public:
    LatchingMotorValveControlStrategy(const std::shared_ptr<PwmMotorDriver>& controller, milliseconds switchDuration, milliseconds brakeDuration, double switchDuty = 1.0)
        : MotorValveControlStrategy(controller)
        , switchDuration(switchDuration)
        , brakeDuration(brakeDuration)
        , switchDuty(switchDuty) {
    }

    void open() override {
        pulse(MotorPhase::Forward);
    }

    void close() override {
        pulse(MotorPhase::Reverse);
    }

    TargetState getDefaultState() const override {
        return TargetState::Closed;
    }

    std::string describe() const override {
        return "latching with switch duration " + std::to_string(switchDuration.count()) + " ms, brake duration " + std::to_string(brakeDuration.count()) + " ms and switch duty " + kernel::toStringWithPrecision(switchDuty * 100, 1) + "%";
    }

private:
    /**
     * @brief Priority to run the pulse at.
     *
     * @details Above every application task, esp-mqtt and pthreads (5), so that MQTT/TLS and telemetry work
     * triggered by the same command can't delay the task-timed parts of the pulse: the brake tail, releasing
     * the driver, and the whole pulse on drivers that don't time it in hardware. Below lwIP (18), esp_timer (22)
     * and WiFi (23).
     */
    static constexpr UBaseType_t PULSE_PRIORITY = 10;

    void pulse(MotorPhase phase) {
        TaskPriorityGuard priorityGuard(PULSE_PRIORITY);
        // End the pulse with a brake instead of coasting: the coil's stored energy then
        // decays inside the bridge rather than flying back onto the load rail (see #581).
        controller->drivePulse(phase, switchDuty, switchDuration);
        Task::delay(brakeDuration);
        controller->stop();
    }

    const milliseconds switchDuration;
    const milliseconds brakeDuration;
    const double switchDuty;
};

class LatchingPinValveControlStrategy
    : public ValveControlStrategy {
public:
    LatchingPinValveControlStrategy(const PinPtr& pin)
        : pin(pin) {
        pin->pinMode(Pin::Mode::Output);
    }

    void open() override {
        pin->digitalWrite(1);
    }

    void close() override {
        pin->digitalWrite(0);
    }

    TargetState getDefaultState() const override {
        return TargetState::Closed;
    }

    std::string describe() const override {
        return "latching with pin " + pin->getName();
    }

private:
    PinPtr pin;
};

}    // namespace cornucopia::ugly_duckling::peripherals::valve
