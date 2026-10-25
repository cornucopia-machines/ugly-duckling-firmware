#pragma once
#include <FunctionRef.hpp>
#include <Log.hpp>
#include <Result.hpp>
#include <config/Configuration.hpp>

#include <concepts>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace cornucopia::ugly_duckling::kernel {

// Forward declarations and common types
struct ShutdownParameters {
    // Placeholder for future parameters
};

// shutdown capability for implementations that support graceful shutdown
class HasShutdown {
public:
    virtual ~HasShutdown() = default;
    virtual void shutdown(const ShutdownParameters& params) = 0;
};

// Generic, TU-stable type tokens: address of per-type inline variable
template <typename T>
inline constexpr char TypeTokenVar = 0;

// A reusable, shutdown-agnostic, type-erased handle that keeps a shared_ptr to an implementation
// and provides tryGet<T>() via compile-time type tokens. Lifecycle operations like shutdown are
// intentionally kept out of this generic type and should be orchestrated by domain managers.
// A single handle can be registered under multiple interface types to support peripherals that
// implement several APIs.
class Handle {
public:
    Handle() = default;

    // Register under explicit interface types. Each interface gets its own correctly-adjusted
    // pointer (important for multiple inheritance). If no Interfaces are given, registers as Impl.
    template <typename... Interfaces, typename Impl>
    static Handle wrap(const std::shared_ptr<Impl>& impl) {
        Handle h;

        if constexpr (sizeof...(Interfaces) == 0) {
            h.entries.push_back({ &TypeTokenVar<Impl>, std::static_pointer_cast<void>(impl) });
        } else {
            (h.entries.push_back({ &TypeTokenVar<Interfaces>,
                 std::static_pointer_cast<void>(std::static_pointer_cast<Interfaces>(impl)) }),
                ...);
        }

        // If implementation supports shutdown, register it with the manager now
        if constexpr (std::is_base_of_v<HasShutdown, Impl>) {
            h.shutdownFn = [impl](const ShutdownParameters& p) {
                std::static_pointer_cast<HasShutdown>(impl)->shutdown(p);
            };
        }

        return h;
    }

    // Typed access without RTTI
    template <typename T>
    std::shared_ptr<T> tryGet() const {
        return std::static_pointer_cast<T>(tryGetByTag(&TypeTokenVar<T>));
    }

    // Linear scan over registered interfaces; kept out of tryGet<T>() so it exists once in flash
    std::shared_ptr<void> tryGetByTag(const void* typeTag) const {
        for (const auto& entry : entries) {
            if (entry.typeTag == typeTag) {
                return entry.holder;
            }
        }
        return {};
    }

    void shutdown(const ShutdownParameters& p) {
        if (shutdownFn) {
            shutdownFn(p);
        }
    }

private:
    struct Entry {
        const void* typeTag;
        std::shared_ptr<void> holder;
    };

    std::vector<Entry> entries;
    std::function<void(const ShutdownParameters& p)> shutdownFn;
};

// A lightweight, generic factory descriptor. The CreateFn is the concrete callable type
// (often a std::function) that returns a Result<Handle> from domain-specific parameters.
template <typename CreateFn>
struct Factory {
    std::string factoryType;    // key used for registration
    std::string productType;    // human-readable/type-identifying string
    CreateFn create;            // callable to create Handle
};

template <typename FactoryT>
class Manager {
public:
    Manager(std::string managed)
        : managed(std::move(managed)) {
    }

    void registerFactory(FactoryT factory) {
        LOGV("Registering %s factory: %s", managed.c_str(), factory.factoryType.c_str());
        factories.emplace(factory.factoryType, std::move(factory));
    }

    template <typename T>
    Result<std::shared_ptr<T>> getInstance(const std::string& name) const {
        auto instance = TRY(getInstanceByTag(name, &TypeTokenVar<T>));
        return std::static_pointer_cast<T>(std::move(instance));
    }

    void shutdown() {
        std::scoped_lock lock(mutex);
        if (state == State::Stopped) {
            return;
        }
        LOGI("Shutting down %s manager",
            managed.c_str());
        state = State::Stopped;
        ShutdownParameters parameters = {};
        for (auto& [name, instance] : instances) {
            LOGI("Shutting down %s '%s'",
                managed.c_str(), name.c_str());
            instance.shutdown(parameters);
        }
    }

    Result<> createWithFactory(
        const std::string& name,
        const std::string& type,
        FunctionRef<Result<Handle>(const FactoryT&)> make) {
        std::scoped_lock lock(mutex);
        if (state == State::Stopped) {
            return failure("Not creating " + managed + " because the manager is stopped");
        }

        LOGD("Creating %s '%s' with factory '%s'",
            managed.c_str(), name.c_str(), type.c_str());
        auto it = factories.find(type);
        if (it == factories.end()) {
            return failure("Factory for '" + type + "' not found");
        }
        const auto& factory = it->second;
        Handle instance = TRY(make(factory));
        instances.emplace(name, std::move(instance));
        return {};
    }

protected:
    const std::string managed;

private:
    // Kept out of getInstance<T>() so it exists once per manager in flash, not once per requested type
    Result<std::shared_ptr<void>> getInstanceByTag(const std::string& name, const void* typeTag) const {
        std::scoped_lock lock(mutex);
        auto it = instances.find(name);
        if (it != instances.end()) {
            auto instance = it->second.tryGetByTag(typeTag);
            if (instance == nullptr) {
                return failure("Instance '" + name + "' is not of the required type");
            }
            return instance;
        }
        return failure("Instance '" + name + "' not found");
    }

    std::map<std::string, FactoryT> factories;
    mutable std::recursive_mutex mutex;
    std::unordered_map<std::string, Handle> instances;

    enum class State : uint8_t {
        Running,
        Stopped
    };

    State state = State::Running;
};

template <typename FactoryT>
class SettingsBasedManager
    : public Manager<FactoryT> {
public:
    SettingsBasedManager(std::string managed)
        : Manager<FactoryT>(std::move(managed)) {
    }

    Result<> createFromSettings(
        const std::string& settingsAsString,
        JsonObject initJson,
        FunctionRef<Result<Handle>(const std::string&, const FactoryT&, const std::string&)> make) {
        LOGI("Creating %s with settings: %s",
            this->managed.c_str(), settingsAsString.c_str());
        std::shared_ptr<ProductSettings> settings = std::make_shared<ProductSettings>();
        auto loaded = settings->loadFromString(settingsAsString);
        if (!loaded) {
            return failure(
                "Failed to parse " + this->managed + " settings because " + loaded.error().message + ":\n" + settingsAsString);
        }

        const auto& name = settings->name.get();
        initJson["name"] = name;
        initJson["type"] = settings->type.get();
        auto created = this->createWithFactory(name, settings->type.get(), [&](const FactoryT& factory) {
            initJson["factory"] = factory.factoryType;
            return make(name, factory, settings->params.get().get());
        });
        if (!created) {
            return failure("Failed to create " + this->managed + " '" + name + "' because: " + created.error().message);
        }
        return {};
    }

private:
    class ProductSettings : public ConfigurationSection {
    public:
        Property<std::string> name { this, "name" };
        Property<std::string> type { this, "type" };
        Property<JsonAsString> params { this, "params" };
    };
};

}    // namespace cornucopia::ugly_duckling::kernel
