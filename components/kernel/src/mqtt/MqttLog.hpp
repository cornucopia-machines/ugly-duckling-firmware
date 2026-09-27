#pragma once
#include <LogJson.hpp>
#include <Task.hpp>
#include <mqtt/MqttRoot.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace cornucopia::ugly_duckling::kernel::mqtt {

/**
 * Every record carries `session` (the persistent boot count, see BootCounter) and `seq` (a per-boot
 * sequence number assigned when the record is enqueued, see LogRecord). The server orders boots by
 * reception time and records within a boot by `seq`; a gap in `seq` means records were lost
 * (issue #635).
 *
 * Alone among the outbound channels, `log` stayed at QoS 2 with a blocking publish when issue #634
 * moved everything else to QoS 1 fire-and-forget. Not because QoS 2 buys ordering -- it doesn't:
 * esp-mqtt keeps no in-flight window on MQTT 3.1.1, so an outbox retransmit reorders records at
 * either QoS level. It's that the 2s wait keeps one record in flight at a time, and that accident
 * is what makes arrival order match emission order for servers that don't yet order on
 * `session`/`seq`.
 *
 * Both the QoS and the timeout drop once the server orders on `session`/`seq`
 * (cornucopia-app#509). Changing either before then would degrade log ordering with nothing to take
 * over.
 */
class MqttLog {
public:
    static void init(Level publishLevel, uint32_t session, const std::shared_ptr<Queue<LogRecord>>& logRecords, std::shared_ptr<MqttRoot> mqttRoot) {
        Task::loop("mqtt:log", 3072, [publishLevel, session, logRecords, mqttRoot](Task& _task) {
            logRecords->take([&](const LogRecord& record) {
                if (record.level > publishLevel) {
                    return;
                }
                auto length = record.message.length();
                // Remove the level prefix
                constexpr size_t messageStart = 2;
                std::string message;
                // With nothing after the prefix, substr() would throw and terminate the task. Still
                // publish the empty record, though: skipping it would leave a gap in `seq` that the
                // server would report as lost records.
                if (length > messageStart) {
                    // Remove trailing newline
                    auto messageEnd = record.message[length - 1] == '\n'
                        ? length - 1
                        : length;
                    message = record.message.substr(messageStart, messageEnd - messageStart);
                }

                mqttRoot->publish(
                    "log", [session, seq = record.seq, level = record.level, message](JsonObject& json) {
                        json["session"] = session;
                        json["seq"] = seq;
                        json["level"] = level;
                        json["message"] = message;
                    },
                    QoS::ExactlyOnce, 2s, LogPublish::Silent);
            });
        });
    }
};

}    // namespace cornucopia::ugly_duckling::kernel::mqtt
