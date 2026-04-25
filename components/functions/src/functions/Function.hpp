#pragma once

#include <Manager.hpp>
#include <NvsConfiguration.hpp>
#include <NvsStore.hpp>
#include <Telemetry.hpp>

#include <peripherals/Peripheral.hpp>

using cornucopia::ugly_duckling::peripherals::PeripheralManager;

namespace cornucopia::ugly_duckling::functions {

// Fields are kept in alphabetical order — please maintain this when adding new entries.
struct FunctionServices {
    const std::shared_ptr<PeripheralManager> peripherals;
    const std::shared_ptr<TelemetryPublisher> telemetryPublisher;
};

struct FunctionInitParameters {
    std::shared_ptr<MqttRoot> functionRoot() {
        if (!mqttFunctionRoot) {
            mqttFunctionRoot = mqttDeviceRoot->forSuffix("functions/" + name);
        }
        return mqttFunctionRoot;
    }

    template <typename T>
    std::shared_ptr<T> peripheral(const std::string& name) const {
        return services.peripherals->getPeripheral<T>(name);
    }

    const std::string name;
    const FunctionServices& services;
    const std::shared_ptr<MqttRoot> mqttDeviceRoot;
    std::shared_ptr<MqttRoot> mqttFunctionRoot;
};

using FunctionCreateFn = std::function<Handle(
    FunctionInitParameters& params,
    const std::shared_ptr<NvsStore>& nvs,
    const std::string& jsonSettings,
    JsonObject& initConfigJson)>;
using FunctionFactory = kernel::Factory<FunctionCreateFn>;

// Helper to build a FunctionFactory while keeping strong types for settings/config
template <
    typename Impl,
    std::derived_from<ConfigurationSection> TSettings,
    std::derived_from<ConfigurationSection> TConfig = EmptyConfiguration,
    typename... TSettingsArgs>
FunctionFactory makeFunctionFactory(
    const std::string& type,
    std::function<std::shared_ptr<Impl>(const FunctionInitParameters&, const std::shared_ptr<TSettings>&)> makeImpl,
    TSettingsArgs... settingsArgs) {
    auto settingsTuple = std::make_tuple(std::forward<TSettingsArgs>(settingsArgs)...);

    // Build the factory using designated initializers (C++20+)
    return FunctionFactory {
        .factoryType = type,
        .productType = std::move(type),
        .create = [settingsTuple, makeImpl = std::move(makeImpl)](
                      FunctionInitParameters& params,
                      const std::shared_ptr<NvsStore>& nvs,
                      const std::string& jsonSettings,
                      JsonObject& initConfigJson) -> Handle {
            // Construct and load settings
            auto settings = std::apply([](auto&&... a) {
                return std::make_shared<TSettings>(std::forward<decltype(a)>(a)...);
            },
                settingsTuple);
            settings->loadFromString(jsonSettings);

            constexpr bool hasConfig = std::is_base_of_v<HasConfig<TConfig>, Impl>;

            // We load configuration up front to ensure that we always store it in the init message, even
            // when the instantiation of the function fails later.
            auto config = std::make_shared<TConfig>();
            std::shared_ptr<NvsConfiguration<TConfig>> nvsConfig;
            if constexpr (hasConfig) {
                nvsConfig = std::make_shared<NvsConfiguration<TConfig>>(nvs, params.name, config);
                // Store configuration in init message
                config->store(initConfigJson);
            }

            // Create concrete implementation via user-provided callable
            auto impl = makeImpl(params, settings);

            // Configuration lifecycle, mirroring the templated factory behavior
            if constexpr (hasConfig) {
                std::static_pointer_cast<HasConfig<TConfig>>(impl)->configure(config);

                // Subscribe for config updates
                params.functionRoot()->subscribe("config", [name = params.name, nvsConfig, impl](const std::string&, const JsonObject& cfgJson) {
                    LOGD("Received configuration update for function: %s", name.c_str());
                    try {
                        nvsConfig->update(cfgJson);
                        if constexpr (std::is_base_of_v<HasConfig<TConfig>, Impl>) {
                            std::static_pointer_cast<HasConfig<TConfig>>(impl)->configure(nvsConfig->getConfig());
                        }
                    } catch (const std::exception& e) {
                        LOGE("Failed to update configuration for function '%s' because %s", name.c_str(), e.what());
                    }
                });
            }

            return Handle::wrap(std::move(impl));
        },
    };
}

class FunctionManager final {
public:
    FunctionManager(
        const std::shared_ptr<NvsStore>& nvs,
        const FunctionServices& services,
        const std::shared_ptr<MqttRoot>& mqttDeviceRoot)
        : nvs(nvs)
        , services(services)
        , mqttDeviceRoot(mqttDeviceRoot)
        , manager("function") {
    }

    bool createFunction(const std::string& functionSettings, JsonArray functionsInitJson) {
        auto initJson = functionsInitJson.add<JsonObject>();
        try {
            manager.createFromSettings(
                functionSettings,
                initJson,
                [&](const std::string& name, const FunctionFactory& factory, const std::string& settings) {
                    FunctionInitParameters params = {
                        .name = name,
                        .services = services,
                        .mqttDeviceRoot = mqttDeviceRoot,
                    };
                    JsonObject initConfigJson = initJson["config"].to<JsonObject>();
                    return factory.create(params, nvs, settings, initConfigJson);
                });
            return true;
        } catch (const std::exception& e) {
            LOGE("%s",
                e.what());
            initJson["error"] = std::string(e.what());
            return false;
        }
    }

    void registerFactory(FunctionFactory factory) {
        manager.registerFactory(std::move(factory));
    }

    void shutdown() {
        manager.shutdown();
    }

private:
    const std::shared_ptr<NvsStore> nvs;
    const FunctionServices services;
    const std::shared_ptr<MqttRoot>& mqttDeviceRoot;

    SettingsBasedManager<FunctionFactory> manager;
};

}    // namespace cornucopia::ugly_duckling::functions
