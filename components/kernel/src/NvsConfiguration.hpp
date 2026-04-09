#pragma once

#include <functional>
#include <memory>

#include <ArduinoJson.h>

#include "Configuration.hpp"
#include "NvsStore.hpp"

namespace cornucopia::ugly_duckling::kernel {

/**
 * @brief Loads a ConfigurationSection from NVS, and persists updates back to NVS.
 */
template <std::derived_from<ConfigurationSection> TConfiguration>
class NvsConfiguration {
public:
    NvsConfiguration(std::shared_ptr<NvsStore> nvs, const std::string& key, std::shared_ptr<TConfiguration> config)
        : nvs(std::move(nvs))
        , key(key)
        , config(std::move(config)) {
        JsonDocument doc;
        if (this->nvs->getJson(key, doc)) {
            this->config->load(doc.as<JsonObject>());
            LOGD("Loaded NVS config for '%s'", key.c_str());
        } else {
            LOGD("No NVS config found for '%s', using defaults", key.c_str());
        }
    }

    void update(const JsonObject& json) {
        config->load(json);
        if (!nvs->setJson(key, json)) {
            LOGE("Failed to save NVS config for '%s'", key.c_str());
        }
    }

    std::shared_ptr<TConfiguration> getConfig() const {
        return config;
    }

    void store(JsonObject& json) const {
        config->store(json);
    }

private:
    std::shared_ptr<NvsStore> nvs;
    const std::string key;
    std::shared_ptr<TConfiguration> config;
};

/**
 * @brief Loads a ConfigurationSection from NVS by key.
 * Returns default-constructed config if key is absent or cannot be parsed.
 */
template <std::derived_from<ConfigurationSection> TConfiguration, typename... TArgs>
std::shared_ptr<TConfiguration> loadConfigFromNvs(const std::shared_ptr<NvsStore>& nvs, const std::string& key, TArgs&&... args) {
    auto config = std::make_shared<TConfiguration>(std::forward<TArgs>(args)...);
    JsonDocument doc;
    if (nvs->getJson(key, doc)) {
        config->load(doc.as<JsonObject>());
        LOGD("Loaded NVS config for '%s'", key.c_str());
    } else {
        LOGD("No NVS config found for '%s', using defaults", key.c_str());
    }
    return config;
}

}    // namespace cornucopia::ugly_duckling::kernel
