#pragma once

#include <chrono>
#include <memory>
#include <utility>

#include <Configuration.hpp>
#include <Named.hpp>
#include <functions/Function.hpp>
#include <functions/ScheduledTransitionLoop.hpp>

#include <peripherals/api/IDoor.hpp>
#include <scheduling/CompositeScheduler.hpp>
#include <scheduling/DelayScheduler.hpp>
#include <scheduling/LightSensorScheduler.hpp>
#include <scheduling/OverrideScheduler.hpp>

using namespace farmhub::peripherals::api;
using namespace farmhub::utils::scheduling;

namespace farmhub::functions::chicken_door {

LOGGING_TAG(CHICKEN_DOOR, "chicken-door")

struct LightTarget : ConfigurationSection {
    /**
     * @brief Light level above which the door should be opened.
     */
    Property<Lux> open { this, "open", 250 };

    /**
     * @brief Light level below which the door should be closed.
     */
    Property<Lux> close { this, "close", 10 };
};

struct DelayTarget : ConfigurationSection {
    Property<seconds> open { this, "open", 0s };

    /**
     * @brief Delay before closing the door after the close condition is met.
     */
    Property<seconds> close { this, "close", 0s };
};

struct ChickenDoorConfig : ConfigurationSection {
    /**
     * @brief Light levels to open or close the door at.
     */
    NamedConfigurationEntry<LightTarget> lightTarget { this, "lightTarget" };

    /**
     * @brief Delays after opening or closing the door.
     */
    NamedConfigurationEntry<DelayTarget> delayTarget { this, "delayTarget" };

    /**
     * @brief The state to override the schedule with.
     */
    Property<TargetState> overrideState { this, "overrideState" };

    /**
     * @brief Until when the override state is valid.
     */
    Property<time_point<system_clock>> overrideUntil { this, "overrideUntil" };
};

class ChickenDoor final
    : public Named,
      public HasConfig<ChickenDoorConfig> {
public:
    ChickenDoor(
        const std::string& name,
        const std::shared_ptr<IDoor>& door,
        const std::shared_ptr<ILightSensor>& lightSensor,
        const std::shared_ptr<TelemetryPublisher>& telemetryPublisher)
        : Named(name) {
        LOGTI(CHICKEN_DOOR, "Initializing chicken-door '%s' with door '%s'",
            name.c_str(),
            door->getName().c_str());

        auto overrideScheduler = std::make_shared<OverrideScheduler>();
        auto lightSensorScheduler = std::make_shared<LightSensorScheduler>(lightSensor);
        auto delayScheduler = std::make_shared<DelayScheduler>(lightSensorScheduler);

        auto compositeScheduler = std::make_shared<CompositeScheduler>(
            overrideScheduler,
            delayScheduler);

        runScheduledTransitionLoop<IDoor, ConfigSpec>(
            name,
            CHICKEN_DOOR,
            door,
            compositeScheduler,
            telemetryPublisher,
            configQueue,
            [overrideScheduler, lightSensorScheduler, delayScheduler](const ConfigSpec& config) {
                overrideScheduler->setOverride(config.overrideTarget);
                lightSensorScheduler->setTarget(config.lightTarget);
                delayScheduler->setTarget(config.delayTarget);
            });
    }

    void configure(const std::shared_ptr<ChickenDoorConfig>& config) override {
        auto overrideState = config->overrideState.getIfPresent();
        auto overrideTarget = overrideState.has_value()
            ? std::make_optional<OverrideSchedule>({
                  .state = *overrideState,
                  .until = config->overrideUntil.get(),
              })
            : std::nullopt;
        configQueue.put(ConfigSpec {
            .overrideTarget = overrideTarget,
            .lightTarget = {
                .open = config->lightTarget.get()->open.get(),
                .close = config->lightTarget.get()->close.get(),
            },
            .delayTarget = {
                .open = config->delayTarget.get()->open.get(),
                .close = config->delayTarget.get()->close.get(),
            },
        });
    }

private:
    struct ConfigSpec {
        std::optional<OverrideSchedule> overrideTarget;
        LightSensorSchedule lightTarget;
        DelaySchedule delayTarget;
    };
    Queue<ConfigSpec> configQueue { "configQueue", 1 };
};

struct ChickenDoorSettings : ConfigurationSection {
    Property<std::string> door { this, "door" };
    Property<std::string> lightSensor { this, "lightSensor" };
};

struct NoOpLightSensor : virtual ILightSensor, Named {
    NoOpLightSensor(const std::string& name)
        : Named(name) {
    }

    Lux getLightLevel() override {
        return -999;
    }

    const std::string& getName() const override {
        return Named::name;
    }
};

inline FunctionFactory makeFactory() {
    return makeFunctionFactory<ChickenDoor, ChickenDoorSettings, ChickenDoorConfig>(
        "chicken-door",
        [](const FunctionInitParameters& params, const std::shared_ptr<ChickenDoorSettings>& settings) {
            auto door = params.peripheral<IDoor>(settings->door.get());
            auto lightSensor = settings->lightSensor.hasValue()
                ? params.peripheral<ILightSensor>(settings->lightSensor.get())
                : std::make_shared<NoOpLightSensor>(params.name + ":light");
            return std::make_shared<ChickenDoor>(
                params.name,
                door,
                lightSensor,
                params.services.telemetryPublisher);
        });
}

}    // namespace farmhub::functions::chicken_door
