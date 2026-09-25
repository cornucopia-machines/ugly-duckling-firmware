#include "NvsStore.hpp"
#include "mqtt/MqttRoot.hpp"
#include <HttpUpdate.hpp>
#include <Log.hpp>
#include <MqttCommands.hpp>
#include <Restart.hpp>

#include <esp_sleep.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <ratio>
#include <string>

using namespace std::chrono;
using namespace cornucopia::ugly_duckling::kernel;

void registerBasicCommands(const std::shared_ptr<MqttRoot>& mqttRoot) {
    mqttRoot->registerCommand("restart", [](const JsonObject&, JsonObject&) {
        printf("Restarting...\n");
        delayedRestart();
    });
    mqttRoot->registerCommand("sleep", [](const JsonObject& request, JsonObject& _response) {
        seconds duration = seconds(request["duration"].as<int64_t>());
        esp_sleep_enable_timer_wakeup(duration_cast<std::chrono::duration<uint64_t, std::micro>>(duration).count());
        LOGI("Sleeping deep for %lld seconds",
            duration.count());
        esp_deep_sleep_start();
    });
}

void registerNvsCommands(const std::shared_ptr<MqttRoot>& mqttRoot) {
    mqttRoot->registerCommand("nvs/list", [](const JsonObject& request, JsonObject& response) {
        const char* ns = request["namespace"] | "config";
        NvsStore store(ns);
        JsonArray entries = response["entries"].to<JsonArray>();
        store.list([entries](const std::string& key) {
            auto entry = entries.add<JsonObject>();
            entry["key"] = key;
        });
    });
    mqttRoot->registerCommand("nvs/read", [](const JsonObject& request, JsonObject& response) {
        const char* ns = request["namespace"] | "config";
        NvsStore store(ns);
        auto key = request["key"].as<std::string>();
        LOGI("Reading NVS key '%s' from namespace '%s'", key.c_str(), ns);
        response["key"] = key;
        JsonDocument valueDoc;
        if (store.getJson(key, valueDoc)) {
            response["value"].set(valueDoc.as<JsonVariant>());
        } else {
            response["error"] = "Key not found";
        }
    });
    mqttRoot->registerCommand("nvs/write", [](const JsonObject& request, JsonObject& response) {
        const char* ns = request["namespace"] | "config";
        NvsStore store(ns);
        auto key = request["key"].as<std::string>();
        LOGI("Writing NVS key '%s' to namespace '%s'", key.c_str(), ns);
        response["key"] = key;
        store.setJson(key, request["value"]);
        response["written"] = true;
    });
    mqttRoot->registerCommand("nvs/remove", [](const JsonObject& request, JsonObject& response) {
        const char* ns = request["namespace"] | "config";
        NvsStore store(ns);
        auto key = request["key"].as<std::string>();
        LOGI("Removing NVS key '%s' from namespace '%s'", key.c_str(), ns);
        response["key"] = key;
        if (store.remove(key)) {
            response["removed"] = true;
        } else {
            response["error"] = "Key not found or could not be removed";
        }
    });
}

void registerHttpUpdateCommand(const std::shared_ptr<MqttRoot>& mqttRoot, const std::shared_ptr<NvsStore>& nvs) {
    mqttRoot->registerCommand("update", [nvs](const JsonObject& request, JsonObject& response) {
        if (!request["url"].is<std::string>()) {
            response["failure"] = "Command contains no URL";
            return;
        }
        std::string url = request["url"];
        if (url.empty()) {
            response["failure"] = "Command contains empty url";
            return;
        }
        HttpUpdater::startUpdate(url, nvs);
        response["success"] = true;
    });
}
