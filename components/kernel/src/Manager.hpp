#pragma once

#include <concepts>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>

#include <Concurrent.hpp>
#include <Configuration.hpp>

namespace farmhub::kernel {

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
// and provides tryGet<T>() via a compile-time type token. Lifecycle operations like shutdown are
// intentionally kept out of this generic type and should be orchestrated by domain managers.
class Handle {
public:
    Handle() = default;

    template <typename ImplPtr>
    static Handle wrap(const ImplPtr& impl) {
        Handle h;
        using Impl = std::remove_reference_t<decltype(*impl)>;
        // Store the impl as void and record a per-type token
        h._holder = std::static_pointer_cast<void>(impl);
        h._typeTag = &TypeTokenVar<Impl>;

        // If implementation supports shutdown, register it with the manager now
        if constexpr (std::is_base_of_v<HasShutdown, Impl>) {
            h._shutdown = ([impl](const ShutdownParameters& p) {
                std::static_pointer_cast<HasShutdown>(impl)->shutdown(p);
            });
        }

        return h;
    }

    // Typed access without RTTI
    template <typename T>
    std::shared_ptr<T> tryGet() const {
        if (_typeTag == &TypeTokenVar<T>) {
            return std::static_pointer_cast<T>(_holder);
        }
        return {};
    }

    void shutdown(const ShutdownParameters& p) {
        if (_shutdown) {
            _shutdown(p);
        }
    }

private:
    std::shared_ptr<void> _holder;
    const void* _typeTag { nullptr };
    std::function<void(const ShutdownParameters& p)> _shutdown;
};

// A lightweight, generic factory descriptor. The CreateFn is the concrete callable type
// (often a std::function) that returns a Handle from domain-specific parameters.
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
    std::shared_ptr<T> getInstance(const std::string& name) const {
        Lock lock(mutex);
        auto it = instances.find(name);
        if (it != instances.end()) {
            auto instance = it->second.template tryGet<T>();
            if (instance == nullptr) {
                throw std::runtime_error("Instance '" + name + "' is not of the required type");
            }
            return instance;
        }
        throw std::runtime_error("Instance '" + name + "' not found");
    }

    void shutdown() {
        Lock lock(mutex);
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
            try {
                instance.shutdown(parameters);
            } catch (const std::exception& e) {
                LOGE("Shutdown of %s '%s' failed: %s",
                    managed.c_str(), name.c_str(), e.what());
            }
        }
    }

    void createWithFactory(
        const std::string& name,
        const std::string& type,
        const std::function<Handle(const FactoryT&)>& make) {
        Lock lock(mutex);
        if (state == State::Stopped) {
            throw std::runtime_error("Not creating " + managed + " because the manager is stopped");
        }

        LOGD("Creating %s '%s' with factory '%s'",
            managed.c_str(), name.c_str(), type.c_str());
        auto it = factories.find(type);
        if (it == factories.end()) {
            throw std::runtime_error("Factory for '" + type + "' not found");
        }
        const auto& factory = it->second;
        Handle instance = make(factory);
        instances.emplace(name, std::move(instance));
    }

protected:
    const std::string managed;

private:
    std::map<std::string, FactoryT> factories;
    mutable RecursiveMutex mutex;
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

    void createFromSettings(
        const std::string& settingsAsString,
        JsonObject initJson,
        const std::function<Handle(const std::string&, const FactoryT&, const std::string&)>& make) {
        LOGI("Creating %s with settings: %s",
            this->managed.c_str(), settingsAsString.c_str());
        std::shared_ptr<ProductSettings> settings = std::make_shared<ProductSettings>();
        try {
            settings->loadFromString(settingsAsString);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "Failed to parse " + this->managed + " settings because " + e.what() + ":\n" + settingsAsString);
        }

        const auto& name = settings->name.get();
        initJson["name"] = name;
        initJson["type"] = settings->type.get();
        settings->params.store(initJson);
        try {
            this->createWithFactory(name, settings->type.get(), [&](const FactoryT& factory) {
                initJson["factory"] = factory.factoryType;
                return make(name, factory, settings->params.get().get());
            });
        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to create " + this->managed + " '" + name + "' because: " + e.what());
        }
    }

private:
    class ProductSettings : public ConfigurationSection {
    public:
        Property<std::string> name { this, "name" };
        Property<std::string> type { this, "type" };
        Property<JsonAsString> params { this, "params" };
    };
};

}    // namespace farmhub::kernel
