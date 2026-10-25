#pragma once
#include <EspError.hpp>
#include <I2CManager.hpp>
#include <Manager.hpp>
#include <Named.hpp>
#include <PulseCounter.hpp>
#include <PwmManager.hpp>
#include <Result.hpp>
#include <Telemetry.hpp>
#include <config/Configuration.hpp>
#include <drivers/SwitchManager.hpp>
#include <mqtt/MqttRoot.hpp>
#include <peripherals/api/IPeripheral.hpp>

#include <concepts>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::drivers;
using namespace cornucopia::ugly_duckling::peripherals::api;

namespace cornucopia::ugly_duckling::peripherals {

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

// Fields are kept in alphabetical order — please maintain this when adding new entries.
struct PeripheralServices {
    const std::shared_ptr<I2CManager> i2c;
    const std::shared_ptr<mqtt::MqttRoot> mqttDeviceRoot;
    const std::shared_ptr<NvsStore> nvs;
    const std::shared_ptr<PulseCounterManager> pulseCounterManager;
    const std::shared_ptr<PwmManager> pwmManager;
    const std::shared_ptr<SwitchManager> switches;
    const std::shared_ptr<TelemetryPublisher> telemetryPublisher;
};

struct PeripheralInitParameters;

using PeripheralCreateFn = std::function<Result<Handle>(
    PeripheralInitParameters& params,
    const std::string& jsonSettings)>;
using PeripheralFactory = kernel::Factory<PeripheralCreateFn>;

struct PeripheralInitParameters {
    void registerFeature(const std::string& type, std::function<void(JsonObject&)> populate) {
        telemetryCollector->registerFeature(type, name, std::move(populate));
        features.add(type);
    }

    std::shared_ptr<mqtt::MqttRoot> peripheralRoot() {
        if (!mqttPeripheralRoot) {
            mqttPeripheralRoot = services.mqttDeviceRoot->forSuffix("peripherals/" + name);
        }
        return mqttPeripheralRoot;
    }

    template <typename T>
    Result<std::shared_ptr<T>> peripheral(const std::string& name) const {
        return peripherals.getInstance<T>(name);
    }

    const std::string name;
    const PeripheralServices& services;
    const std::shared_ptr<TelemetryCollector> telemetryCollector;
    const JsonArray features;

    Manager<PeripheralFactory>& peripherals;
    std::shared_ptr<mqtt::MqttRoot> mqttPeripheralRoot = nullptr;
};

// Helper to build a PeripheralFactory while keeping strong types for settings/config.
// RegisterAs... lists the interface types to register the peripheral under. If empty,
// the peripheral is registered under its concrete Impl type.
// makeImpl may return either the peripheral, or a Result of it when creating it can fail.
template <
    typename Impl,
    std::derived_from<ConfigurationSection> TSettings,
    typename... RegisterAs>
    requires(sizeof...(RegisterAs) == 0 || (std::derived_from<Impl, RegisterAs> && ...))
PeripheralFactory makePeripheralFactory(const std::string& factoryType, const std::string& peripheralType, std::function<Result<std::shared_ptr<Impl>>(PeripheralInitParameters&, const std::shared_ptr<TSettings>&)> makeImpl, std::function<std::shared_ptr<TSettings>()> makeSettings = [] { return std::make_shared<TSettings>(); }) {

    // Build the factory using designated initializers (C++20+)
    auto effectiveType = peripheralType.empty() ? factoryType : peripheralType;
    return PeripheralFactory {
        .factoryType = std::move(factoryType),
        .productType = std::move(effectiveType),
        .create = [makeImpl = std::move(makeImpl), makeSettings = std::move(makeSettings)](
                      PeripheralInitParameters& params,
                      const std::string& jsonSettings) -> Result<Handle> {
            // Construct and load settings
            auto settings = makeSettings();
            TRY(settings->loadFromString(jsonSettings));

            // Create concrete implementation via user-provided callable
            auto impl = TRY(makeImpl(params, settings));
            return Handle::wrap<RegisterAs...>(std::move(impl));
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
        auto created = manager.createFromSettings(
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
        if (!created) {
            LOGE("%s",
                created.error().message.c_str());
            initJson["error"] = created.error().message;
            return false;
        }
        return true;
    }

    void registerFactory(PeripheralFactory factory) {
        manager.registerFactory(std::move(factory));
    }

    template <typename T>
    Result<std::shared_ptr<T>> getPeripheral(const std::string& name) const {
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

}    // namespace cornucopia::ugly_duckling::peripherals
