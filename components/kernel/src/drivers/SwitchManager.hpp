#pragma once

#include <chrono>
#include <functional>
#include <utility>

#include <driver/gpio.h>
#include <hal/gpio_types.h>

#include <Concurrent.hpp>
#include <Pin.hpp>
#include <Task.hpp>

using namespace std::chrono;

using farmhub::kernel::PinPtr;

namespace farmhub::kernel::drivers {

LOGGING_TAG(SWITCH, "switch")

enum class SwitchMode : uint8_t {
    PullUp,
    PullDown
};

class Switch {
public:
    virtual ~Switch() = default;

    virtual const std::string& getName() const = 0;
    virtual InternalPinPtr getPin() const = 0;
    virtual bool isEngaged() const = 0;
};

struct SwitchStateChange {
    gpio_num_t gpio;
    bool engaged;
    milliseconds timeSinceLastChange;
};

struct SwitchEvent {
    std::shared_ptr<Switch> switchState;
    bool engaged;
    milliseconds timeSinceLastChange;
};

static void handleSwitchInterrupt(void* arg);

class SwitchManager final {
public:
    using SwitchEventHandler = std::function<void(const SwitchEvent&)>;

    struct SwitchConfig {
        std::string name;
        InternalPinPtr pin;
        SwitchMode mode;
        SwitchEventHandler onEngaged = nullptr;
        SwitchEventHandler onDisengaged = nullptr;
        milliseconds debounceTime = 50ms;
    };

    SwitchManager() {
        Task::loop("switch-manager", 3072, [this](Task& _task) {
            SwitchStateChange stateChange = switchStateInterrupts.take();
            std::shared_ptr<SwitchState> state;
            {
                Lock lock(switchStatesMutex);
                auto it = switchStates.find(stateChange.gpio);
                if (it == switchStates.end()) {
                    LOGTE(SWITCH, "Switch state change for unknown GPIO %d", stateChange.gpio);
                    return;
                }
                state = it->second;
            }

            auto engaged = stateChange.engaged;
            auto timeSinceLastChange = stateChange.timeSinceLastChange;

            LOGTD(SWITCH, "Switch %s is %s",
                state->name.c_str(), engaged ? "engaged" : "disengaged");
            if (engaged) {
                if (state->engageHandler) {
                    state->engageHandler(SwitchEvent {
                        .switchState = state,
                        .engaged = engaged,
                        .timeSinceLastChange = timeSinceLastChange,
                    });
                }
            } else {
                if (state->disengageHandler) {
                    state->disengageHandler(SwitchEvent {
                        .switchState = state,
                        .engaged = engaged,
                        .timeSinceLastChange = timeSinceLastChange,
                    });
                }
            }
        });
    }

    std::shared_ptr<Switch> registerSwitch(const SwitchConfig& config) {
        LOGTI(SWITCH, "Registering switch %s on pin %s, mode %s, debounce %lld ms",
            config.name.c_str(), config.pin->getName().c_str(),
            config.mode == SwitchMode::PullUp ? "pull-up" : "pull-down",
            config.debounceTime.count());

        // Configure PIN_INPUT as input
        config.pin->pinMode(config.mode == SwitchMode::PullUp ? Pin::Mode::InputPullUp : Pin::Mode::InputPullDown);

        auto switchState = std::make_shared<SwitchState>(
            config.name,
            config.pin,
            config.mode,
            this,
            config.onEngaged,
            config.onDisengaged,
            config.debounceTime);
        {
            Lock lock(switchStatesMutex);
            switchStates.emplace(config.pin->getGpio(), switchState);
        }

        // Install GPIO ISR
        ESP_ERROR_THROW(gpio_set_intr_type(config.pin->getGpio(), GPIO_INTR_ANYEDGE));
        ESP_ERROR_THROW(gpio_isr_handler_add(config.pin->getGpio(), handleSwitchInterrupt, switchState.get()));

        return switchState;
    }

private:
    struct SwitchState final : public Switch {
    public:
        SwitchState(const std::string& name, const InternalPinPtr& pin, SwitchMode mode, SwitchManager* manager,
            SwitchEventHandler engageHandler, SwitchEventHandler disengageHandler, milliseconds debounceTime)
            : name(name)
            , pin(pin)
            , mode(mode)
            , manager(manager)
            , engageHandler(std::move(engageHandler))
            , disengageHandler(std::move(disengageHandler))
            , debounceTime(debounceTime)
            , lastChangeTime(steady_clock::now())
            , lastReportedState(isEngaged()) {
        }

        const std::string& getName() const override {
            return name;
        }

        InternalPinPtr getPin() const override {
            return pin;
        }

        bool isEngaged() const override {
            return pin->digitalRead() == (mode == SwitchMode::PullUp ? 0 : 1);
        }

    private:
        std::string name;
        InternalPinPtr pin;
        SwitchMode mode;
        SwitchManager* manager;

        SwitchEventHandler engageHandler;
        SwitchEventHandler disengageHandler;

        milliseconds debounceTime;
        steady_clock::time_point lastChangeTime;
        bool lastReportedState;

        friend class SwitchManager;
        friend void handleSwitchInterrupt(void* arg);
    };

    Mutex switchStatesMutex;
    std::unordered_map<gpio_num_t, std::shared_ptr<SwitchState>> switchStates;

    CopyQueue<SwitchStateChange> switchStateInterrupts { "switchState-state-interrupts", 1 };
    friend void handleSwitchInterrupt(void* arg);
};

// ISR handler for GPIO interrupt
static void IRAM_ATTR handleSwitchInterrupt(void* arg) {
    auto* state = static_cast<SwitchManager::SwitchState*>(arg);

    // Must use gpio_get_level() to read the pin state instead of pin->digitalRead()
    // because we cannot call virtual methods from an ISR
    auto gpio = state->pin->getGpio();
    bool engaged = gpio_get_level(gpio) == (state->mode == SwitchMode::PullUp ? 0 : 1);

    // Ignore if the state hasn't actually changed from what we last reported
    if (engaged == state->lastReportedState) {
        return;
    }

    // Software debounce: ignore state changes that happen too quickly
    auto now = steady_clock::now();
    auto timeSinceLastChange = duration_cast<milliseconds>(now - state->lastChangeTime);
    if (timeSinceLastChange < state->debounceTime) {
        return;
    }

    // Update state tracking
    state->lastChangeTime = now;
    state->lastReportedState = engaged;

    // Use overwriteFromISR to ensure we never lose the latest state change
    state->manager->switchStateInterrupts.overwriteFromISR(SwitchStateChange {
        .gpio = gpio,
        .engaged = engaged,
        .timeSinceLastChange = timeSinceLastChange,
    });
}

}    // namespace farmhub::kernel::drivers
