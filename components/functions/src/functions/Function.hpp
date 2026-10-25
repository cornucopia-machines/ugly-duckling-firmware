#pragma once
#include <Manager.hpp>
#include <NvsStore.hpp>
#include <Telemetry.hpp>
#include <config/ConfigEnvelope.hpp>
#include <config/StoredConfig.hpp>
#include <functions/FunctionConfigTracker.hpp>
#include <peripherals/Peripheral.hpp>

#include <functional>
#include <memory>
#include <string>

using cornucopia::ugly_duckling::kernel::config::ConfigEnvelope;
using cornucopia::ugly_duckling::kernel::config::StoredConfig;
using cornucopia::ugly_duckling::peripherals::PeripheralManager;

namespace cornucopia::ugly_duckling::functions {

// Fields are kept in alphabetical order — please maintain this when adding new entries.
struct FunctionServices {
    const std::shared_ptr<PeripheralManager> peripherals;
    const std::shared_ptr<TelemetryPublisher> telemetryPublisher;
};

struct FunctionInitParameters {
    template <typename T>
    Result<std::shared_ptr<T>> peripheral(const std::string& name) const {
        return services.peripherals->getPeripheral<T>(name);
    }

    const std::string name;
    const FunctionServices& services;
};

struct FunctionCreationResult {
    Handle handle;
    ConfigureFn configureFn;    // empty if the function doesn't implement HasConfig<TConfig>
    std::string fingerprint;    // empty if no config, or none was found in NVS at boot
    std::string requestedAt;    // empty if no config, or none was found in NVS at boot
};

using FunctionCreateFn = std::function<Result<FunctionCreationResult>(
    FunctionInitParameters& params,
    const std::shared_ptr<NvsStore>& nvs,
    const std::string& jsonSettings)>;
using FunctionFactory = kernel::Factory<FunctionCreateFn>;

// Helper to build a FunctionFactory while keeping strong types for settings/config.
// makeImpl may return either the function, or a Result of it when creating it can fail.
// makeImpl is captured as-is rather than as a std::function: wrapping it would add a second
// type-erasure layer inside `create`, costing a few hundred bytes of flash per factory.
template <
    typename Impl,
    std::derived_from<ConfigurationSection> TSettings,
    std::derived_from<ConfigurationSection> TConfig = EmptyConfiguration,
    typename MakeImpl,
    typename... TSettingsArgs>
    requires std::is_convertible_v<std::invoke_result_t<const MakeImpl&, const FunctionInitParameters&, const std::shared_ptr<TSettings>&>, Result<std::shared_ptr<Impl>>>
FunctionFactory makeFunctionFactory(
    const std::string& type,
    MakeImpl makeImpl,
    TSettingsArgs... settingsArgs) {
    auto settingsTuple = std::make_tuple(std::forward<TSettingsArgs>(settingsArgs)...);

    // Build the factory using designated initializers (C++20+)
    return FunctionFactory {
        .factoryType = type,
        .productType = std::move(type),
        .create = [settingsTuple, makeImpl = std::move(makeImpl)](
                      FunctionInitParameters& params,
                      const std::shared_ptr<NvsStore>& nvs,
                      const std::string& jsonSettings) -> Result<FunctionCreationResult> {
            // Construct and load settings
            auto settings = std::apply([](auto&&... a) {
                return std::make_shared<TSettings>(std::forward<decltype(a)>(a)...);
            },
                settingsTuple);
            TRY(settings->loadFromString(jsonSettings));

            constexpr bool hasConfig = std::is_base_of_v<HasConfig<TConfig>, Impl>;

            // We load configuration up front so it's available for the fingerprint/requestedAt below
            // even when the instantiation of the function fails later.
            std::shared_ptr<TConfig> config = std::make_shared<TConfig>();
            std::shared_ptr<StoredConfig> storedConfig;
            if constexpr (hasConfig) {
                storedConfig = std::make_shared<StoredConfig>(nvs, params.name);
                if (storedConfig->hasValue()) {
                    JsonDocument dataCopy = storedConfig->data();
                    TRY(config->load(dataCopy.as<JsonObject>()));
                }
            }

            // Create concrete implementation via user-provided callable
            Result<std::shared_ptr<Impl>> created = makeImpl(params, settings);
            auto impl = TRY(std::move(created));

            FunctionCreationResult result;
            if constexpr (hasConfig) {
                std::static_pointer_cast<HasConfig<TConfig>>(impl)->configure(config);
                if (storedConfig->hasValue()) {
                    result.fingerprint = storedConfig->fingerprint();
                    result.requestedAt = storedConfig->requestedAt();
                }

                result.configureFn = [impl](JsonObjectConst data) -> Result<> {
                    auto parsed = std::make_shared<TConfig>();
                    JsonDocument copy;
                    copy.set(data);
                    TRY(parsed->load(copy.as<JsonObject>()));
                    std::static_pointer_cast<HasConfig<TConfig>>(impl)->configure(parsed);
                    return {};
                };
            }

            result.handle = Handle::wrap(std::move(impl));
            return result;
        },
    };
}

/**
 * @brief In-memory source of truth for name -> {handle, fingerprint} across every live function.
 *
 * Evolved from FunctionManager per docs/Configuration.md: applyLive() is the hot-reload entry
 * point that applies an already-persisted envelope to a running function, recording the
 * fingerprint/requestedAt only once configure() succeeds (proof-of-apply, not proof-of-receipt);
 * manifest() reports name -> {fingerprint, requestedAt} straight from this in-memory state for the
 * SYNC builder. The apply-and-track-fingerprint bookkeeping itself lives in FunctionConfigTracker,
 * which has no NVS dependency and is unit-tested directly.
 * Persistence is deliberately not this class's job: a functions-only `UPDATE` persists the whole
 * staged slot up front, once, the same way a device-changed `UPDATE` does (docs/Configuration.md,
 * "Applying a functions-only UPDATE") -- applyLive() only applies the change to the already-running
 * function. It's called by the `…/update` handler in Device.hpp's registerUpdateHandler(), for its
 * functions-only branch.
 */
class FunctionRegistry final {
public:
    FunctionRegistry(
        const std::shared_ptr<NvsStore>& nvs,
        const FunctionServices& services)
        : nvs(nvs)
        , services(services)
        , manager("function") {
    }

    bool createFunction(const std::string& functionSettings, JsonArray functionsInitJson) {
        auto initJson = functionsInitJson.add<JsonObject>();
        auto created = manager.createFromSettings(
            functionSettings,
            initJson,
            [&](const std::string& name, const FunctionFactory& factory, const std::string& settings) -> Result<Handle> {
                FunctionInitParameters params = {
                    .name = name,
                    .services = services,
                };
                FunctionCreationResult result = TRY(factory.create(params, nvs, settings));
                tracker.record(name, std::move(result.configureFn), result.fingerprint, result.requestedAt);
                return result.handle;
            });
        if (!created) {
            LOGE("%s",
                created.error().message.c_str());
            initJson["error"] = created.error().message;
            return false;
        }
        return true;
    }

    // Hot-reload entry point: applies the envelope to the running function and records the
    // fingerprint via the tracker, only once configureFn succeeds (proof-of-apply). The envelope
    // itself is already persisted -- by the caller, once, as part of writing the whole staged slot
    // (docs/Configuration.md, "Applying a functions-only UPDATE") -- so a faulty body failing here
    // leaves NVS on the not-yet-committed staged slot and the in-memory fingerprint on the last-good
    // one; the caller reacts to the failure by marking the attempt rejected and rebooting to revert.
    // Fails on any error (unknown function name, a function without configuration, or a faulty
    // body) -- there is no cause-classified rejection yet, so this is handled exactly like any
    // other faulty configuration today.
    Result<> applyLive(const std::string& name, const ConfigEnvelope& envelope) {
        return tracker.apply(name, envelope.getData().template as<JsonObjectConst>(), envelope.getFingerprint(), envelope.getRequestedAt());
    }

    // name -> {fingerprint, requestedAt} for every live function, straight from in-memory state --
    // never re-reading NVS, so it reflects what actually applied.
    std::unordered_map<std::string, ConfigManifestEntry> manifest() const {
        return tracker.manifest();
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

    SettingsBasedManager<FunctionFactory> manager;
    FunctionConfigTracker tracker;
};

}    // namespace cornucopia::ugly_duckling::functions
