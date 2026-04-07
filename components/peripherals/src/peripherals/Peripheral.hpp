#pragma once

#include <functional>
#include <map>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <Configuration.hpp>
#include <EspException.hpp>
#include <I2CManager.hpp>
#include <Manager.hpp>
#include <Named.hpp>
#include <PcntManager.hpp>
#include <PulseCounter.hpp>
#include <PwmManager.hpp>
#include <Telemetry.hpp>
#include <drivers/SwitchManager.hpp>

#include <peripherals/api/IPeripheral.hpp>

#include "PeripheralException.hpp"

using namespace farmhub::kernel;
using namespace farmhub::kernel::drivers;
using namespace farmhub::peripherals::api;

namespace farmhub::peripherals {

class Peripheral
    : public virtual IPeripheral,
      public Named {
public:
    Peripheral(const std::string& name)
        : Named(name) {
    }

    const std::string& getName() const override {
        return Named::name;
    }
};

// Peripheral factories

struct PeripheralServices {
    const std::shared_ptr<I2CManager> i2c;
    const std::shared_ptr<NvsStore> nvs;
    const std::shared_ptr<PcntManager> pcntManager;
    const std::shared_ptr<PulseCounterManager> pulseCounterManager;
    const std::shared_ptr<PwmManager> pwmManager;
    const std::shared_ptr<SwitchManager> switches;
    const std::shared_ptr<TelemetryPublisher> telemetryPublisher;
};

struct PeripheralInitParameters;

using PeripheralCreateFn = std::function<Handle(
    PeripheralInitParameters& params,
    const std::string& jsonSettings)>;
using PeripheralFactory = kernel::Factory<PeripheralCreateFn>;

struct PeripheralInitParameters {
    void registerFeature(const std::string& type, std::function<void(JsonObject&)> populate) {
        telemetryCollector->registerFeature(type, name, std::move(populate));
        features.add(type);
    }

    template <typename T>
    std::shared_ptr<T> peripheral(const std::string& name) const {
        return peripherals.getInstance<T>(name);
    }

    const std::string name;
    const PeripheralServices& services;
    const std::shared_ptr<TelemetryCollector> telemetryCollector;
    const JsonArray features;

    Manager<PeripheralFactory>& peripherals;
};

// Helper to build a PeripheralFactory while keeping strong types for settings/config
template <
    typename Type,
    std::derived_from<Type> Impl,
    std::derived_from<ConfigurationSection> TSettings,
    typename... TSettingsArgs>
PeripheralFactory makePeripheralFactory(const std::string& factoryType,
    const std::string& peripheralType,
    std::function<std::shared_ptr<Impl>(PeripheralInitParameters&, const std::shared_ptr<TSettings>&)> makeImpl,
    TSettingsArgs... settingsArgs) {
    auto settingsTuple = std::make_tuple(std::forward<TSettingsArgs>(settingsArgs)...);

    // Build the factory using designated initializers (C++20+)
    auto effectiveType = peripheralType.empty() ? factoryType : peripheralType;
    return PeripheralFactory {
        .factoryType = std::move(factoryType),
        .productType = std::move(effectiveType),
        .create = [settingsTuple, makeImpl = std::move(makeImpl)](
                      PeripheralInitParameters& params,
                      const std::string& jsonSettings) -> Handle {
            // Construct and load settings
            auto settings = std::apply([](auto&&... a) {
                return std::make_shared<TSettings>(std::forward<decltype(a)>(a)...);
            },
                settingsTuple);
            settings->loadFromString(jsonSettings);

            // Create concrete implementation via user-provided callable
            auto impl = makeImpl(params, settings);
            return Handle::wrap(std::move(std::static_pointer_cast<Type>(impl)));
        },
    };
}

// Peripheral manager

class PeripheralManager final {
public:
    PeripheralManager(
        const std::shared_ptr<TelemetryCollector>& telemetryCollector,
        PeripheralServices services)
        : telemetryCollector(telemetryCollector)
        , services(std::move(services))
        , manager("peripheral") {
    }

    bool createPeripheral(const std::string& peripheralSettings, JsonArray peripheralsInitJson) {
        auto initJson = peripheralsInitJson.add<JsonObject>();
        try {
            manager.createFromSettings(
                peripheralSettings,
                initJson,
                [&](const std::string& name, const PeripheralFactory& factory, const std::string& settings) {
                    PeripheralInitParameters params = {
                        .name = name,
                        .services = services,
                        .telemetryCollector = telemetryCollector,
                        .features = initJson["features"].to<JsonArray>(),
                        .peripherals = manager,
                    };
                    return factory.create(params, settings);
                });
            return true;
        } catch (const std::exception& e) {
            LOGE("%s",
                e.what());
            initJson["error"] = std::string(e.what());
            return false;
        }
    }

    void registerFactory(PeripheralFactory factory) {
        manager.registerFactory(std::move(factory));
    }

    template <typename T>
    std::shared_ptr<T> getPeripheral(const std::string& name) const {
        return manager.getInstance<T>(name);
    }

    void shutdown() {
        manager.shutdown();
    }

private:
    const std::shared_ptr<TelemetryCollector> telemetryCollector;
    const PeripheralServices services;

    SettingsBasedManager<PeripheralFactory> manager;
};

}    // namespace farmhub::peripherals
