#pragma once
#include <Result.hpp>

#include <ArduinoJson.h>

#include <chrono>
#include <concepts>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using std::ref;
using std::reference_wrapper;

namespace cornucopia::ugly_duckling::kernel::config {

/**
 * @brief Lets a property reject a value after ArduinoJson converted it, e.g. a pin name that names no
 * pin on this board. Specialize it for types whose conversion can fail.
 */
template <typename T>
struct PropertyValidator {
    static Result<> validate(const T& /*value*/, JsonVariantConst /*json*/) {
        return {};
    }
};

class JsonAsString {
public:
    JsonAsString() = default;

    JsonAsString(const std::string& value)
        : value(value) {
    }

    JsonAsString(const JsonAsString& other)
        = default;

    JsonAsString& operator=(const JsonAsString& other) = default;

    const std::string& get() const {
        return value;
    }

    void set(const std::string& value) {
        this->value = value;
    }

private:
    std::string value;
};

class ConfigurationEntry {
public:
    virtual ~ConfigurationEntry() = default;

    Result<> loadFromString(const std::string& json) {
        JsonDocument jsonDocument;
        DeserializationError error = deserializeJson(jsonDocument, json);
        if (error == DeserializationError::EmptyInput) {
            return {};
        }
        if (error) {
            return failure("Cannot parse JSON configuration: " + std::string(error.c_str()) + ": " + json);
        }
        return load(jsonDocument.as<JsonObject>());
    }

    virtual Result<> load(const JsonObject& json) = 0;
    virtual bool hasValue() const = 0;
};

class ConfigurationSection : public ConfigurationEntry {
public:
    void add(ConfigurationEntry& entry) {
        auto reference = std::ref(entry);
        entries.push_back(reference);
    }

    Result<> load(const JsonObject& json) override {
        for (auto& entry : entries) {
            TRY(entry.get().load(json));
        }
        return {};
    }

    bool hasValue() const override {
        for (const auto& entry : entries) {
            if (entry.get().hasValue()) {
                return true;
            }
        }
        return false;
    }

private:
    std::vector<reference_wrapper<ConfigurationEntry>> entries;
};

class EmptyConfiguration : public ConfigurationSection { };

// Interface indicating the implementation supports configuration via TConfig
template <std::derived_from<ConfigurationSection> TConfig>
class HasConfig {
public:
    virtual ~HasConfig() = default;
    virtual void configure(const std::shared_ptr<TConfig>&) = 0;
};

// The type-independent part of NamedConfigurationEntry, so it exists once in flash instead of once
// per delegate type: loading and hasValue() only go through ConfigurationEntry's virtual methods.
class NamedConfigurationEntryBase : public ConfigurationEntry {
public:
    Result<> load(const JsonObject& json) override {
        if (json[name].is<JsonObject>()) {
            namePresentAtLoad = true;
            auto loaded = delegate->load(json[name]);
            if (!loaded) {
                return failure("In '" + name + "': " + loaded.error().message);
            }
        }
        return {};
    }

    bool hasValue() const override {
        return namePresentAtLoad || delegate->hasValue();
    }

protected:
    NamedConfigurationEntryBase(ConfigurationSection* parent, const std::string& name, std::shared_ptr<ConfigurationEntry> delegate)
        : name(name)
        , delegate(std::move(delegate)) {
        parent->add(*this);
    }

    const std::shared_ptr<ConfigurationEntry>& getDelegate() const {
        return delegate;
    }

private:
    const std::string name;
    const std::shared_ptr<ConfigurationEntry> delegate;
    bool namePresentAtLoad = false;
};

template <std::derived_from<ConfigurationEntry> TDelegateEntry>
class NamedConfigurationEntry : public NamedConfigurationEntryBase {
public:
    NamedConfigurationEntry(ConfigurationSection* parent, const std::string& name, std::shared_ptr<TDelegateEntry> delegate)
        : NamedConfigurationEntryBase(parent, name, std::move(delegate)) {
    }

    template <typename... Args>
        requires std::constructible_from<TDelegateEntry, Args...>
    NamedConfigurationEntry(ConfigurationSection* parent, const std::string& name, Args&&... args)
        : NamedConfigurationEntry(parent, name, std::make_shared<TDelegateEntry>(std::forward<Args>(args)...)) {
    }

    std::shared_ptr<TDelegateEntry> get() const {
        // The delegate is always a TDelegateEntry: the constructors only accept one
        return std::static_pointer_cast<TDelegateEntry>(getDelegate());
    }
};

template <typename T>
class Property : public ConfigurationEntry {
public:
    Property(ConfigurationSection* parent, const std::string& name, const T& defaultValue = T())
        : name(name)
        , value(defaultValue)
        , defaultValue(defaultValue) {
        parent->add(*this);
    }

    T get() const {
        return getOrDefault(defaultValue);
    }

    T getOrDefault(const T& defaultValue) const {
        return configured ? value : defaultValue;
    }

    std::optional<T> getIfPresent() const {
        if (configured) {
            return value;
        }
        return std::nullopt;
    }

    Result<> load(const JsonObject& json) override {
        JsonVariantConst jsonValue = json[name];
        if (jsonValue.template is<T>()) {
            T converted = jsonValue.template as<T>();
            auto valid = PropertyValidator<T>::validate(converted, jsonValue);
            if (!valid) {
                return failure("Invalid '" + name + "': " + valid.error().message);
            }
            value = std::move(converted);
            configured = true;
        }
        return {};
    }

    bool hasValue() const override {
        return configured;
    }

private:
    const std::string name;
    bool configured = false;
    T value;
    const T defaultValue;
};

template <typename T>
class ArrayProperty : public ConfigurationEntry {
public:
    ArrayProperty(ConfigurationSection* parent, const std::string& name)
        : name(name) {
        parent->add(*this);
    }

    const std::vector<T>& get() const {
        return entries;
    }

    Result<> load(const JsonObject& json) override {
        if (json[name].template is<JsonArray>()) {
            auto jsonArray = json[name].template as<JsonArray>();
            for (auto jsonEntry : jsonArray) {
                T entry = jsonEntry.template as<T>();
                auto valid = PropertyValidator<T>::validate(entry, jsonEntry);
                if (!valid) {
                    return failure("Invalid entry in '" + name + "': " + valid.error().message);
                }
                entries.push_back(std::move(entry));
            }
        }
        return {};
    }

    bool hasValue() const override {
        return !entries.empty();
    }

private:
    const std::string name;
    std::vector<T> entries;
};

}    // namespace cornucopia::ugly_duckling::kernel::config

// ConfigurationSection/Property/etc. are used unqualified throughout the peripherals/functions/
// devices tree, exactly as when they lived directly in `kernel` -- re-exporting them here (rather
// than touching every one of those call sites) mirrors how KernelStatus.hpp re-exports
// `kernel::mqtt` for anything that includes it.
using namespace cornucopia::ugly_duckling::kernel::config;

namespace ArduinoJson {

using namespace std::chrono;

template <typename T>
concept Duration = requires { typename T::rep; typename T::period; }
    && std::is_same_v<T, std::chrono::duration<typename T::rep, typename T::period>>
    && std::is_integral_v<typename T::rep>;

template <Duration D>
struct Converter<D> {
    static bool toJson(const D& src, JsonVariant dst) {
        return dst.set(static_cast<int64_t>(src.count()));
    }

    static D fromJson(JsonVariantConst src) {
        return D { src.as<int64_t>() };
    }

    static bool checkJson(JsonVariantConst src) {
        return src.is<int64_t>();
    }
};

using cornucopia::ugly_duckling::kernel::config::JsonAsString;

template <>
struct Converter<JsonAsString> {
    static bool toJson(const JsonAsString& src, JsonVariant dst) {
        const std::string& stringValue = src.get();
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, stringValue);

        if (error) {
            // Handle the error, if JSON parsing fails
            return false;
        }

        dst.set(doc.as<JsonObject>());
        return true;
    }

    static JsonAsString fromJson(JsonVariantConst src) {
        std::string value;
        serializeJson(src, value);
        return { value };
    }

    static bool checkJson(JsonVariantConst _src) {
        return true;
    }
};

}    // namespace ArduinoJson
