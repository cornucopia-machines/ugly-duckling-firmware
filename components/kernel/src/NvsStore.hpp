#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nvs.h>
#include <nvs_flash.h>

#include <ArduinoJson.h>

namespace cornucopia::ugly_duckling::kernel {

LOGGING_TAG(NVS, "nvs")

/**
 * @brief NVS store for JSON serializable objects.
 */
class NvsStore {
public:
    NvsStore(const std::string& ns)
        : ns(ns) {
    }

    bool contains(const std::string& key) {
        return contains(key.c_str());
    }

    bool contains(const char* key) {
        return withPreferences(true, [&](nvs_handle_t handle) {
            size_t length = 0;
            esp_err_t err = nvs_get_blob(handle, key, nullptr, &length);
            switch (err) {
                case ESP_OK:
                case ESP_ERR_NVS_NOT_FOUND:
                    break;
                default:
                    LOGTW(NVS, "contains(%s) = failed to read: %s", key, esp_err_to_name(err));
                    break;
            }
            return err;
        }) == ESP_OK;
    }

    template <typename T>
    bool get(const std::string& key, T& value) {
        return get(key.c_str(), value);
    }

    template <typename T>
    bool get(const char* key, T& value) {
        JsonDocument doc;
        if (!getJson(key, doc)) {
            return false;
        }
        value = doc.as<T>();
        return true;
    }

    template <typename T>
    bool set(const std::string& key, const T& value) {
        return set(key.c_str(), value);
    }

    template <typename T>
    bool set(const char* key, const T& value) {
        JsonDocument doc;
        doc.set(value);
        return setJson(key, doc.as<JsonVariantConst>());
    }

    bool remove(const std::string& key) {
        return remove(key.c_str());
    }

    bool remove(const char* key) {
        return withPreferences(false, [&](nvs_handle_t handle) {
            LOGTV(NVS, "remove(%s)", key);
            esp_err_t err = nvs_erase_key(handle, key);
            if (err != ESP_OK) {
                LOGTE(NVS, "remove(%s) = cannot delete: %s", key, esp_err_to_name(err));
                return err;
            }
            return nvs_commit(handle);
        }) == ESP_OK;
    }

    bool getJson(const std::string& key, JsonDocument& doc) {
        return getJson(key.c_str(), doc);
    }

    bool getJson(const char* key, JsonDocument& doc) {
        return withPreferences(true, [&](nvs_handle_t handle) -> esp_err_t {
            size_t length = 0;
            esp_err_t err = nvs_get_blob(handle, key, nullptr, &length);
            if (err != ESP_OK) {
                LOGTV(NVS, "getJson(%s) = not found: %s", key, esp_err_to_name(err));
                return err;
            }
            std::vector<char> buffer(length);
            err = nvs_get_blob(handle, key, buffer.data(), &length);
            if (err != ESP_OK) {
                LOGTE(NVS, "getJson(%s) = failed to read: %s", key, esp_err_to_name(err));
                return err;
            }
            DeserializationError jsonError = deserializeJson(doc, buffer.data(), length);
            if (jsonError) {
                LOGTE(NVS, "getJson(%s) = invalid JSON: %s", key, jsonError.c_str());
                return ESP_FAIL;
            }
            LOGTV(NVS, "getJson(%s) = OK", key);
            return ESP_OK;
        }) == ESP_OK;
    }

    bool setJson(const std::string& key, const JsonVariantConst& value) {
        return setJson(key.c_str(), value);
    }

    bool setJson(const char* key, const JsonVariantConst& value) {
        return withPreferences(false, [&](nvs_handle_t handle) -> esp_err_t {
            size_t size = measureJson(value);
            // +1 for null terminator written by serializeJson
            std::vector<char> buffer(size + 1);
            serializeJson(value, buffer.data(), buffer.size());
            LOGTV(NVS, "setJson(%s) = %s", key, buffer.data());
            esp_err_t err = nvs_set_blob(handle, key, buffer.data(), size);
            if (err != ESP_OK) {
                LOGTE(NVS, "setJson(%s) = failed to write: %s", key, esp_err_to_name(err));
                return err;
            }
            return nvs_commit(handle);
        }) == ESP_OK;
    }

    /**
     * @brief Erases all entries in this namespace.
     */
    bool eraseAll() {
        return withPreferences(false, [&](nvs_handle_t handle) {
            LOGTV(NVS, "eraseAll()");
            esp_err_t err = nvs_erase_all(handle);
            if (err != ESP_OK) {
                LOGTE(NVS, "eraseAll() = failed: %s", esp_err_to_name(err));
                return err;
            }
            return nvs_commit(handle);
        }) == ESP_OK;
    }

    /**
     * @brief Enumerates all keys in this namespace.
     */
    void list(const std::function<void(const std::string&)>& callback) {
        nvs_iterator_t it = nullptr;
        esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns.c_str(), NVS_TYPE_ANY, &it);
        while (err == ESP_OK) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            if (std::string(info.namespace_name) == ns) {
                callback(info.key);
            }
            err = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
    }

private:
    esp_err_t withPreferences(bool readOnly, const std::function<esp_err_t(nvs_handle_t)>& action) {
        LOGTV(NVS, "%s '%s'", readOnly ? "read" : "write", ns.c_str());

        nvs_handle_t handle;
        esp_err_t err = nvs_open(ns.c_str(), readOnly ? NVS_READONLY : NVS_READWRITE, &handle);
        switch (err) {
            case ESP_OK:
                break;
            case ESP_ERR_NVS_NOT_FOUND:
                LOGTV(NVS, "namespace '%s' does not exist yet, nothing to read",
                    ns.c_str());
                return ESP_ERR_NOT_FOUND;
                break;
            default:
                LOGTW(NVS, "failed to open NVS to %s '%s': %s",
                    readOnly ? "read" : "write", ns.c_str(), esp_err_to_name(err));
                break;
        }

        esp_err_t result = action(handle);
        nvs_close(handle);

        LOGTV(NVS, "finished %s '%s', result: %s",
            readOnly ? "read" : "write", ns.c_str(), esp_err_to_name(result));
        return result;
    }

    const std::string ns;
};

}    // namespace cornucopia::ugly_duckling::kernel
