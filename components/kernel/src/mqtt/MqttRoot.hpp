#pragma once

#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>

#include <mqtt/MqttDriver.hpp>
#include <utility>

namespace cornucopia::ugly_duckling::kernel::mqtt {

class MqttRoot {
public:
    MqttRoot(const std::shared_ptr<MqttDriver>& mqtt, const std::string& rootTopic)
        : mqtt(mqtt)
        , rootTopic(rootTopic) {
        const std::string commandsTopic = fullTopic("commands/#");
        const auto commandsPrefixLength = commandsTopic.length() - 1;
        mqtt->subscribe(commandsTopic, QoS::ExactlyOnce, [this, commandsPrefixLength](const std::string& topic, const JsonObject& request) {
            std::string command = topic.substr(commandsPrefixLength);
            auto it = commandHandlers.find(command);
            if (it != commandHandlers.end()) {
                JsonDocument responseDoc;
                auto response = responseDoc.to<JsonObject>();
                it->second(request, response);
                if (response.size() > 0) {
                    publish("responses/" + command, responseDoc, Retention::NoRetain, QoS::ExactlyOnce);
                }
            } else {
                std::string knownCommands;
                for (const auto& [name, _] : commandHandlers) {
                    if (!knownCommands.empty()) {
                        knownCommands += ", ";
                    }
                    knownCommands += name;
                }
                LOGTE(MQTT, "Unknown command: %s (known: %s)", command.c_str(), knownCommands.c_str());
            }
        });
    }

    std::shared_ptr<MqttRoot> forSuffix(const std::string& suffix) {
        auto child = std::make_shared<MqttRoot>(mqtt, rootTopic + "/" + suffix);
        children.push_back(child);
        return child;
    }

    PublishStatus publish(const std::string& suffix, const JsonDocument& json, Retention retain = Retention::NoRetain, QoS qos = QoS::AtMostOnce, ticks timeout = MqttDriver::MQTT_NETWORK_TIMEOUT, LogPublish log = LogPublish::Log) {
        return mqtt->publish(fullTopic(suffix), json, retain, qos, timeout, log);
    }

    PublishStatus publish(const std::string& suffix, const std::function<void(JsonObject&)>& populate, Retention retain = Retention::NoRetain, QoS qos = QoS::AtMostOnce, ticks timeout = MqttDriver::MQTT_NETWORK_TIMEOUT, LogPublish log = LogPublish::Log) {
        JsonDocument doc;
        JsonObject root = doc.to<JsonObject>();
        populate(root);
        return publish(suffix, doc, retain, qos, timeout, log);
    }

    PublishStatus clear(const std::string& suffix, Retention retain = Retention::NoRetain, QoS qos = QoS::AtMostOnce, ticks timeout = MqttDriver::MQTT_NETWORK_TIMEOUT) {
        return mqtt->clear(fullTopic(suffix), retain, qos, timeout);
    }

    bool subscribe(const std::string& suffix, SubscriptionHandler handler) {
        return subscribe(suffix, QoS::ExactlyOnce, std::move(handler));
    }

    void registerCommand(const std::string& name, const CommandHandler& handler) {
        commandHandlers.emplace(name, handler);
    }

    /**
     * @brief Subscribes to the given topic under the topic prefix.
     *
     * Note that subscription does not support wildcards.
     */
    bool subscribe(const std::string& suffix, QoS qos, SubscriptionHandler handler) {
        return mqtt->subscribe(fullTopic(suffix), qos, std::move(handler));
    }

    const std::shared_ptr<MqttDriver> mqtt;

private:
    std::string fullTopic(const std::string& suffix) const {
        return rootTopic + "/" + suffix;
    }

    const std::string rootTopic;
    std::unordered_map<std::string, CommandHandler> commandHandlers;
    std::vector<std::shared_ptr<MqttRoot>> children;
};

}    // namespace cornucopia::ugly_duckling::kernel::mqtt
