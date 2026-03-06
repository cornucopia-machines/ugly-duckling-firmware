#pragma once

#include <list>
#include <memory>
#include <utility>

#include <ArduinoJson.h>

#include <Concurrent.hpp>
#include <Log.hpp>

namespace farmhub::kernel {

class TelemetryCollector {
public:
    void collect(JsonArray& featuresJson) {
        for (auto& feature : features) {
            auto featureJson = featuresJson.add<JsonObject>();
            featureJson["type"] = feature.type;
            if (!feature.name.empty()) {
                featureJson["name"] = feature.name;
            }
            auto data = featureJson["data"].to<JsonObject>();
            feature.populate(data);
        }
    }

    void registerFeature(
        const std::string& type,
        const std::string& name,
        std::function<void(JsonObject&)> populate) {
        LOGV("Registering '%s' feature '%s'",
            type.c_str(), name.c_str());
        features.push_back({ type, name, std::move(populate) });
    }

private:
    struct Feature {
        std::string type;
        std::string name;
        std::function<void(JsonObject&)> populate;
    };

    std::list<Feature> features;
};

class TelemetryPublisher {
public:
    TelemetryPublisher(const std::shared_ptr<CopyQueue<bool>>& telemetryPublishQueue)
        : telemetryPublishQueue(telemetryPublishQueue) {}

    void requestTelemetryPublishing() {
        telemetryPublishQueue->overwrite(true);
    }

private:
    std::shared_ptr<CopyQueue<bool>> telemetryPublishQueue;
};

}    // namespace farmhub::kernel
