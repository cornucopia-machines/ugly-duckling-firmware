#pragma once

#include <chrono>
#include <concepts>
#include <limits>
#include <map>
#include <utility>
#include <variant>

#include <Concurrent.hpp>
#include <Named.hpp>
#include <Overloaded.hpp>
#include <Task.hpp>
#include <Telemetry.hpp>
#include <Watchdog.hpp>

#include <drivers/MotorDriver.hpp>
#include <drivers/SwitchManager.hpp>

#include <peripherals/Motors.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/api/IDoor.hpp>

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::drivers;
using namespace cornucopia::ugly_duckling::peripherals;
using namespace cornucopia::ugly_duckling::peripherals::api;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace cornucopia::ugly_duckling::peripherals::door {

LOGGING_TAG(DOOR, "door")

enum class OperationState : uint8_t {
    Running,
    Stopped,
    WatchdogTimeout,
};

/**
 * @brief Manages the motor, watchdog, and movement state for a door.
 * Ensures atomic operations on these three related pieces of state.
 */
class DoorMotorController {
public:
    DoorMotorController(
        const std::shared_ptr<PwmMotorDriver>& motor,
        Watchdog& watchdog)
        : motor(motor)
        , watchdog(watchdog) {
    }

    void drive(MotorPhase phase) {
        motor->drive(phase, 1);
        watchdog.restart();
        moving = true;
    }

    void stop() {
        motor->stop();
        watchdog.cancel();
        moving = false;
    }

    bool isMoving() const {
        return moving;
    }

private:
    const std::shared_ptr<PwmMotorDriver> motor;
    Watchdog& watchdog;
    bool moving = false;
};

class DoorSettings final
    : public ConfigurationSection {
public:
    /**
     * @brief The motor to drive the door.
     */
    Property<std::string> motor { this, "motor" };

    /**
     * @brief Pin that indicates the door is open.
     */
    Property<InternalPinPtr> openPin { this, "openPin" };

    /**
     * @brief Pin that indicates the door is closed.
     */
    Property<InternalPinPtr> closedPin { this, "closedPin" };

    /**
     * @brief By default, open/closed pins are high-active; set this to true to invert the logic.
     */
    Property<bool> invertSwitches { this, "invertSwitches", false };

    /**
     * @brief How long the motor is allowed to be running before we switch to emergency mode.
     */
    Property<seconds> movementTimeout { this, "movementTimeout", seconds(60) };
};

class Door final
    : public api::IDoor,
      public Peripheral,
      public HasShutdown {
public:
    Door(
        const std::string& name,
        const std::shared_ptr<SwitchManager>& switches,
        const std::shared_ptr<PwmMotorDriver>& motor,
        const InternalPinPtr& openPin,
        const InternalPinPtr& closedPin,
        bool invertSwitches,
        ticks movementTimeout,
        const std::shared_ptr<TelemetryPublisher>& telemetryPublisher)
        : Peripheral(name)
        , motor(motor)
        , openSwitch(switches->registerSwitch({
              .name = name + ":open",
              .pin = openPin,
              .mode = invertSwitches ? SwitchMode::PullDown : SwitchMode::PullUp,
              .onEngaged = [this](const SwitchEvent& event) { enqueueSwitchEvent(event); },
              .onDisengaged = [this](const SwitchEvent& event) { enqueueSwitchEvent(event); },
          }))
        , closedSwitch(switches->registerSwitch({
              .name = name + ":closed",
              .pin = closedPin,
              .mode = invertSwitches ? SwitchMode::PullDown : SwitchMode::PullUp,
              .onEngaged = [this](const SwitchEvent& event) { enqueueSwitchEvent(event); },
              .onDisengaged = [this](const SwitchEvent& event) { enqueueSwitchEvent(event); },
          }))
        , watchdog(name + ":watchdog", movementTimeout, false, [this](WatchdogState state) {
            handleWatchdogEvent(state);
        })
        , motorController(motor, watchdog)
        , telemetryPublisher(telemetryPublisher) {

        LOGTI(DOOR, "Initializing door %s, open switch %s, closed switch %s%s",
            name.c_str(), openSwitch->getPin()->getName().c_str(), closedSwitch->getPin()->getName().c_str(),
            invertSwitches ? " (switches are inverted)" : "");

        motorController.stop();

        Task::run(name, 4096, 2, [this](Task& _task) {
            runLoop();
        });
    }

    bool transitionTo(std::optional<TargetState> target) override {
        Lock lock(stateMutex);
        if (this->targetState == target) {
            return false;
        }
        updateQueue.put(ConfigureSpec { .targetState = target });
        return true;
    }

    std::optional<DoorState> getState() override {
        Lock lock(stateMutex);
        return lastState;
    }

    void populateTelemetry(JsonObject& telemetry) {
        Lock lock(stateMutex);
        if (lastState) {
            telemetry["state"] = *lastState;
        }
        if (targetState) {
            telemetry["targetState"] = *targetState;
        }
        telemetry["operationState"] = operationState;
    }

    void shutdown(const ShutdownParameters& _params) override {
        if (operationState == OperationState::Running) {
            updateQueue.put(ShutdownSpec { });
        }
    }

private:
    void runLoop() {
        bool shouldPublishTelemetry = true;
        bool openSwitchEngaged = openSwitch->isEngaged();
        bool closedSwitchEngaged = closedSwitch->isEngaged();
        while (operationState == OperationState::Running) {
            auto currentState = determineCurrentState(openSwitchEngaged, closedSwitchEngaged);
            if (atTargetState(targetState, currentState)) {
                if (motorController.isMoving()) {
                    LOGTD(DOOR, "Door reached target state %s",
                        toString(currentState));
                    motorController.stop();
                    shouldPublishTelemetry = true;
                }
            } else if (targetState) {
                LOGTD(DOOR, "Door moving towards target state %s (current state %s)",
                    toString(targetState), toString(currentState));
                switch (*targetState) {
                    case TargetState::Open:
                        motorController.drive(MotorPhase::Forward);
                        break;
                    case TargetState::Closed:
                        motorController.drive(MotorPhase::Reverse);
                        break;
                }
                shouldPublishTelemetry = true;
            } else {
                LOGTD(DOOR, "Door has no target state, stopping motor (current state %s)",
                    toString(currentState));
                motorController.stop();
                shouldPublishTelemetry = true;
            }

            if (currentState != lastState) {
                Lock lock(stateMutex);
                lastState = currentState;
                shouldPublishTelemetry = true;
            }

            if (shouldPublishTelemetry) {
                telemetryPublisher->requestTelemetryPublishing();
                shouldPublishTelemetry = false;
            }

            updateQueue.take([&](auto& change) {
                std::visit(
                    overloaded {
                        [&](const SwitchEvent& arg) {
                            LOGTV(DOOR, "Status update received: switch=%s, engaged=%s",
                                arg.switchState->getName().c_str(),
                                arg.engaged ? "true" : "false");
                            if (arg.switchState == openSwitch) {
                                openSwitchEngaged = arg.engaged;
                            } else if (arg.switchState == closedSwitch) {
                                closedSwitchEngaged = arg.engaged;
                            }
                        },
                        [&](const ConfigureSpec& arg) {
                            Lock lock(stateMutex);
                            TargetState newTargetState = calculateEffectiveTargetState(arg.targetState, currentState);

                            if (!targetState || *targetState != newTargetState) {
                                LOGTI(DOOR, "Setting target state to %s (current state: %s, last state: %s)",
                                    toString(newTargetState), toString(currentState), toString(lastState));
                                targetState = newTargetState;
                                shouldPublishTelemetry = true;
                            }
                        },
                        [&](const WatchdogTimeout&) {
                            LOGTE(DOOR, "Watchdog timed out, stopping operation");
                            operationState = OperationState::WatchdogTimeout;
                            motorController.stop();
                        },
                        [&](const ShutdownSpec&) {
                            LOGTI(DOOR, "Shutting down door operation");
                            operationState = OperationState::Stopped;
                            motorController.stop();
                        },
                    },
                    change);
            });
        }
        LOGTW(DOOR, "Door '%s' exited run loop",
            name.c_str());
    }

    static bool
    atTargetState(std::optional<TargetState> targetState, std::optional<DoorState> currentState) {
        if (!targetState) {
            return false;
        }
        if (!currentState) {
            return false;
        }
        switch (*targetState) {
            case TargetState::Open:
                return *currentState == DoorState::Open;
            case TargetState::Closed:
                return *currentState == DoorState::Closed;
        }
        std::unreachable();
    }

    static TargetState calculateEffectiveTargetState(std::optional<TargetState> newTargetState, std::optional<DoorState> currentState) {
        if (newTargetState) {
            return *newTargetState;
        }
        if (!currentState) {
            // No target and unknown current state, default to closed for safety
            return TargetState::Closed;
        }

        switch (*currentState) {
            case DoorState::Open:
                return TargetState::Open;
            case DoorState::Closed:
                return TargetState::Closed;
        }
        std::unreachable();
    }

    void handleWatchdogEvent(WatchdogState state) {
        switch (state) {
            case WatchdogState::Started:
                LOGTV(DOOR, "Watchdog started");
                sleepLock.emplace(PowerManager::noLightSleep);
                break;
            case WatchdogState::Cancelled:
                LOGTV(DOOR, "Watchdog cancelled");
                sleepLock.reset();
                break;
            case WatchdogState::TimedOut:
                LOGTV(DOOR, "Watchdog timed out");
                sleepLock.reset();
                updateQueue.put(WatchdogTimeout { });
                break;
        }
    }

    void enqueueSwitchEvent(const SwitchEvent& event) {
        updateQueue.put(event);
    }

    std::optional<DoorState> determineCurrentState(bool open, bool closed) {
        if (open && closed) {
            // TODO Handle this as a failure?
            LOGTW(DOOR, "Both open and closed switches are engaged, should the switches be inverted?");
            return std::nullopt;
        }
        if (open) {
            return DoorState::Open;
        }
        if (closed) {
            return DoorState::Closed;
        }

        if (atTargetState(targetState, lastState)) {
            // We have previously reached the target state, but we have likely lost the signal from the switches.
            // We assume the door is still in the target state to prevent it from moving when it shouldn't.
            return lastState;
        }

        LOGTD(DOOR, "Neither open nor closed switches are engaged");
        return std::nullopt;
    }

    const std::shared_ptr<PwmMotorDriver> motor;

    const std::shared_ptr<Switch> openSwitch;
    const std::shared_ptr<Switch> closedSwitch;

    Watchdog watchdog;
    DoorMotorController motorController;

    const std::shared_ptr<TelemetryPublisher> telemetryPublisher;

    struct ConfigureSpec {
        std::optional<TargetState> targetState;
    };

    struct WatchdogTimeout { };

    struct ShutdownSpec { };

    Queue<std::variant<SwitchEvent, ConfigureSpec, WatchdogTimeout, ShutdownSpec>> updateQueue { "door-status", 8 };

    OperationState operationState = OperationState::Running;

    Mutex stateMutex;
    std::optional<TargetState> targetState;
    std::optional<DoorState> lastState;

    std::optional<PowerManagementLockGuard> sleepLock;
};

inline PeripheralFactory makeFactory(const std::map<std::string, std::shared_ptr<PwmMotorDriver>>& motors) {

    return makePeripheralFactory<IDoor, Door, DoorSettings>(
        "door",
        "door",
        [motors](PeripheralInitParameters& params, const std::shared_ptr<DoorSettings>& settings) {
            auto motor = findMotor(motors, settings->motor.get());

            auto door = std::make_shared<Door>(
                params.name,
                params.services.switches,
                motor,
                settings->openPin.get(),
                settings->closedPin.get(),
                settings->invertSwitches.get(),
                settings->movementTimeout.get(),
                params.services.telemetryPublisher);

            params.registerFeature("door", [door](JsonObject& telemetryJson) {
                door->populateTelemetry(telemetryJson);
            });

            return door;
        });
}

}    // namespace cornucopia::ugly_duckling::peripherals::door

namespace ArduinoJson {

using cornucopia::ugly_duckling::peripherals::door::OperationState;

template <>
struct Converter<OperationState> {
    static bool toJson(const OperationState& src, JsonVariant dst) {
        return dst.set(static_cast<int>(src));
    }

    static OperationState fromJson(JsonVariantConst src) {
        return static_cast<OperationState>(src.as<int>());
    }

    static bool checkJson(JsonVariantConst src) {
        return src.is<int>();
    }
};

}    // namespace ArduinoJson
