#pragma once
#include <LogJson.hpp>
#include <Task.hpp>
#include <mqtt/MqttRoot.hpp>

#include <memory>
#include <string>

namespace cornucopia::ugly_duckling::kernel::mqtt {

/**
 * Alone among the outbound channels, `log` stayed at QoS 2 with a blocking publish when issue #634
 * moved everything else to QoS 1 fire-and-forget. Not because QoS 2 buys ordering -- it doesn't:
 * esp-mqtt keeps no in-flight window on MQTT 3.1.1, so an outbox retransmit reorders records at
 * either QoS level. It's that the 2s wait keeps one record in flight at a time, and that accident
 * is currently the only thing making arrival order match emission order, since the payload carries
 * no sequence of its own.
 *
 * Issue #635 replaces that with an explicit `seq`/`session` in the payload and server-side ordering
 * (cornucopia-app#509); the QoS and the timeout both drop as part of it. Changing either before
 * then would degrade log ordering with nothing to take over.
 */
class MqttLog {
public:
    static void init(Level publishLevel, const std::shared_ptr<Queue<LogRecord>>& logRecords, std::shared_ptr<MqttRoot> mqttRoot) {
        Task::loop("mqtt:log", 3072, [publishLevel, logRecords, mqttRoot](Task& _task) {
            logRecords->take([&](const LogRecord& record) {
                if (record.level > publishLevel) {
                    return;
                }
                auto length = record.message.length();
                // Remove the level prefix
                auto messageStart = 2;
                // Remove trailing newline
                auto messageEnd = record.message[length - 1] == '\n'
                    ? length - 1
                    : length;
                std::string message = record.message.substr(messageStart, messageEnd - messageStart);

                mqttRoot->publish(
                    "log", [level = record.level, message](JsonObject& json) {
                        json["level"] = level;
                        json["message"] = message;
                    },
                    QoS::ExactlyOnce, 2s, Retention::NoRetain, LogPublish::Silent);
            });
        });
    }
};

}    // namespace cornucopia::ugly_duckling::kernel::mqtt
