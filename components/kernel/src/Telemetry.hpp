#pragma once
#include <Log.hpp>
#include <Queue.hpp>
#include <drivers/SwitchedRail.hpp>

#include <ArduinoJson.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cornucopia::ugly_duckling::kernel {

class TelemetryCollector {
public:
    /**
     * @param switchedRail Kept powered for the whole collection, so the sensors on it share one power-up.
     */
    explicit TelemetryCollector(std::shared_ptr<drivers::SwitchedRail> switchedRail)
        : switchedRail(std::move(switchedRail)) {
    }

    void collect(JsonArray& featuresJson) {
        auto railBatch = switchedRail->batch();
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

    const std::shared_ptr<drivers::SwitchedRail> switchedRail;
    std::vector<Feature> features;
};

class TelemetryPublisher {
public:
    TelemetryPublisher(const std::shared_ptr<CopyQueue<bool>>& telemetryPublishQueue)
        : telemetryPublishQueue(telemetryPublishQueue) {
    }

    void requestTelemetryPublishing() {
        telemetryPublishQueue->overwrite(true);
    }

private:
    std::shared_ptr<CopyQueue<bool>> telemetryPublishQueue;
};

}    // namespace cornucopia::ugly_duckling::kernel
