#pragma once

#include <chrono>
#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include <ArduinoJson.h>

using std::ref;
using std::reference_wrapper;

namespace farmhub::kernel {

class ConfigurationException
    : public std::exception {
public:
    ConfigurationException(const std::string& message)
        : message("ConfigurationException: " + message) {
    }

    const char* what() const noexcept override {
        return message.c_str();
    }

    const std::string message;
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

    void loadFromString(const std::string& json) {
        JsonDocument jsonDocument;
        DeserializationError error = deserializeJson(jsonDocument, json);
        if (error == DeserializationError::EmptyInput) {
            return;
        }
        if (error) {
            throw ConfigurationException("Cannot parse JSON configuration: " + std::string(error.c_str()) + ": " + json);
        }
        load(jsonDocument.as<JsonObject>());
    }

    virtual void load(const JsonObject& json) = 0;
    virtual void reset() = 0;
    virtual void store(JsonObject& json) const = 0;
    virtual bool hasValue() const = 0;
};

class ConfigurationSection : public ConfigurationEntry {
public:
    void add(ConfigurationEntry& entry) {
        auto reference = std::ref(entry);
        entries.push_back(reference);
    }

    void load(const JsonObject& json) override {
        for (auto& entry : entries) {
            entry.get().load(json);
        }
    }

    void reset() override {
        for (auto& entry : entries) {
            entry.get().reset();
        }
    }

    void store(JsonObject& json) const override {
        for (const auto& entry : entries) {
            entry.get().store(json);
        }
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

template <std::derived_from<ConfigurationEntry> TDelegateEntry>
class NamedConfigurationEntry : public ConfigurationEntry {
public:
    NamedConfigurationEntry(ConfigurationSection* parent, const std::string& name, std::shared_ptr<TDelegateEntry> delegate)
        : name(name)
        , delegate(std::move(delegate)) {
        parent->add(*this);
    }

    template <typename... Args>
        requires std::constructible_from<TDelegateEntry, Args...>
    NamedConfigurationEntry(ConfigurationSection* parent, const std::string& name, Args&&... args)
        : NamedConfigurationEntry(parent, name, std::make_shared<TDelegateEntry>(std::forward<Args>(args)...)) {
    }

    void load(const JsonObject& json) override {
        if (json[name].is<JsonObject>()) {
            namePresentAtLoad = true;
            delegate->load(json[name]);
        } else {
            reset();
        }
    }

    void store(JsonObject& json) const override {
        if (hasValue()) {
            auto section = json[name].to<JsonObject>();
            delegate->store(section);
        }
    }

    bool hasValue() const override {
        return namePresentAtLoad || delegate->hasValue();
    }

    void reset() override {
        namePresentAtLoad = false;
        delegate->reset();
    }

    std::shared_ptr<TDelegateEntry> get() const {
        return delegate;
    }

private:
    const std::string name;
    const std::shared_ptr<TDelegateEntry> delegate;
    bool namePresentAtLoad = false;
};

template <typename T>
class Property : public ConfigurationEntry {
public:
    Property(ConfigurationSection* parent, const std::string& name, const T& defaultValue = T(), const bool secret = false)
        : name(name)
        , secret(secret)
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

    void load(const JsonObject& json) override {
        if (json[name].is<T>()) {
            value = json[name].as<T>();
            configured = true;
        } else {
            reset();
        }
    }

    bool hasValue() const override {
        return configured;
    }

    void reset() override {
        configured = false;
        value = T();
    }

    void store(JsonObject& json) const override {
        if (!configured) {
            return;
        }
        if (secret) {
            json[name] = "********";
        } else {
            json[name] = get();
        }
    }

private:
    const std::string name;
    const bool secret;
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

    void load(const JsonObject& json) override {
        reset();
        if (json[name].is<JsonArray>()) {
            auto jsonArray = json[name].as<JsonArray>();
            for (auto jsonEntry : jsonArray) {
                const T& entry = jsonEntry.as<T>();
                entries.push_back(entry);
            }
        }
    }

    bool hasValue() const override {
        return !entries.empty();
    }

    void reset() override {
        entries.clear();
    }

    void store(JsonObject& json) const override {
        auto jsonArray = json[name].to<JsonArray>();
        for (auto& entry : entries) {
            jsonArray.add(entry);
        }
    }

private:
    const std::string name;
    std::vector<T> entries;
};

}    // namespace farmhub::kernel

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

using farmhub::kernel::JsonAsString;

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
