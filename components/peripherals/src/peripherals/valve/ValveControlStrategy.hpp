#pragma once

#include <chrono>
#include <cstdint>

#include <Task.hpp>
#include <drivers/MotorDriver.hpp>

#include <peripherals/api/IValve.hpp>

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
        return "normally closed with switch duration " + std::to_string(switchDuration.count()) + " ms and hold duty " + std::to_string(holdDuty * 100) + "%";
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
        return "normally open with switch duration " + std::to_string(switchDuration.count()) + " ms and hold duty " + std::to_string(holdDuty * 100) + "%";
    }
};

class LatchingMotorValveControlStrategy
    : public MotorValveControlStrategy {
public:
    LatchingMotorValveControlStrategy(const std::shared_ptr<PwmMotorDriver>& controller, milliseconds switchDuration, double switchDuty = 1.0)
        : MotorValveControlStrategy(controller)
        , switchDuration(switchDuration)
        , switchDuty(switchDuty) {
    }

    void open() override {
        controller->drive(MotorPhase::Forward, switchDuty);
        Task::delay(switchDuration);
        controller->stop();
    }

    void close() override {
        controller->drive(MotorPhase::Reverse, switchDuty);
        Task::delay(switchDuration);
        controller->stop();
    }

    TargetState getDefaultState() const override {
        return TargetState::Closed;
    }

    std::string describe() const override {
        return "latching with switch duration " + std::to_string(switchDuration.count()) + " ms and switch duty " + std::to_string(switchDuty * 100) + "%";
    }

private:
    const milliseconds switchDuration;
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
