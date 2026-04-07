#pragma once

#include <atomic>
#include <chrono>
#include <list>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <esp_event.h>
#include <mqtt_client.h>

#include <Concurrent.hpp>
#include <Configuration.hpp>
#include <State.hpp>
#include <Task.hpp>
#include <mqtt/PendingMessages.hpp>

using namespace std::chrono;
using namespace std::chrono_literals;
using namespace farmhub::kernel;
using namespace farmhub::kernel::drivers;

namespace farmhub::kernel::mqtt {

LOGGING_TAG(MQTT, "mqtt")

enum class Retention : uint8_t {
    NoRetain,
    Retain
};

enum class QoS : uint8_t {
    AtMostOnce = 0,
    AtLeastOnce = 1,
    ExactlyOnce = 2
};

enum class LogPublish : uint8_t {
    Log,
    Silent
};

using CommandHandler = std::function<void(const JsonObject&, JsonObject&)>;

using SubscriptionHandler = std::function<void(const std::string&, const JsonObject&)>;

class MqttRoot;

class MqttDriver {
public:
    class Config : public ConfigurationSection {
    public:
        Property<std::string> host { this, "host", "" };
        Property<unsigned int> port { this, "port", 1883 };
        Property<std::string> clientId { this, "clientId", "" };
        Property<size_t> queueSize { this, "queueSize", 128 };
        ArrayProperty<std::string> serverCert { this, "serverCert" };
        ArrayProperty<std::string> clientCert { this, "clientCert" };
        ArrayProperty<std::string> clientKey { this, "clientKey" };
    };

    MqttDriver(
        State& networkReady,
        const std::shared_ptr<Config>& config,
        const std::string& instanceName,
        StateSource& ready)
        : networkReady(networkReady)
        , configHostname(config->host.get())
        , configPort(config->port.get())
        , configServerCert(joinStrings(config->serverCert.get()))
        , configClientCert(joinStrings(config->clientCert.get()))
        , configClientKey(joinStrings(config->clientKey.get()))
        , clientId(getClientId(config->clientId.get(), instanceName))
        , ready(ready)
        , eventQueue("mqtt-outgoing", config->queueSize.get())
        , incomingQueue("mqtt-incoming", config->queueSize.get()) {

        Task::run("mqtt", 5120, [this](Task& task) {
            esp_mqtt_client_config_t mqttConfig = {};
            client = esp_mqtt_client_init(&mqttConfig);

            ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, handleMqttEventCallback, this));

            runEventLoop(task);
        });
        Task::loop("mqtt:incoming", 4096, [this](Task& _task) {
            incomingQueue.take([this](const IncomingMessage& message) {
                processIncomingMessage(message);
            });
        });
    }

    State& getReady() {
        return ready;
    }

    void populateTelemetry(JsonObject& json) {
        json["disconnects"] = disconnectCount.exchange(0, std::memory_order_relaxed);
    }

    void configMqttClient(esp_mqtt_client_config_t& config) {
        if (configHostname.empty()) {
#ifdef WOKWI
#ifdef WOKWI_MQTT_HOST
            hostname = WOKWI_MQTT_HOST;
#else
            hostname = "host.wokwi.internal";
#endif
            port = 1883;
#else
            throw std::runtime_error("No MQTT server specified in configuration");
#endif
        } else {
            hostname = configHostname;
            port = configPort;
        }

        config = {
            .broker {
                .address {
                    .uri = nullptr,
                    .hostname = hostname.c_str(),
                    .transport = MQTT_TRANSPORT_OVER_TCP,
                    .path = nullptr,
                    .port = port,
                },
                .verification {},
            },
            .credentials {
                .username = nullptr,
                .client_id = clientId.c_str(),
                .set_null_client_id = false,
                .authentication {},
            },
            // TODO Configure last will
            .session {
                .last_will {},
                .disable_clean_session = false,
                .keepalive = duration_cast<seconds>(MQTT_SESSION_KEEP_ALIVE).count(),
                .disable_keepalive = false,
                .protocol_ver = MQTT_PROTOCOL_UNDEFINED,    // Default MQTT version
                .message_retransmit_timeout = duration_cast<milliseconds>(MQTT_MESSAGE_RETRANSMIT_TIMEOUT).count(),
            },
            .network {
                .reconnect_timeout_ms = duration_cast<milliseconds>(MQTT_CONNECTION_TIMEOUT).count(),
                .timeout_ms = duration_cast<milliseconds>(MQTT_NETWORK_TIMEOUT).count(),
                .refresh_connection_after_ms = 0,    // No need to refresh connection
                .disable_auto_reconnect = false,
                .tcp_keep_alive_cfg = {},
                .transport = nullptr,    // Use default transport
                .if_name = nullptr,      // Use default interface
            },
            .task {},
            .buffer {
                .size = 8192,
                .out_size = 4096,
            },
            .outbox {},
        };

        LOGTD(MQTT, "server: %s:%" PRIu32 ", client ID is '%s'",
            config.broker.address.hostname,
            config.broker.address.port,
            config.credentials.client_id);

        if (!configServerCert.empty()) {
            config.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
            config.broker.verification.certificate = configServerCert.c_str();
            LOGTV(MQTT, "Server cert:\n%s",
                config.broker.verification.certificate);

            if (!configClientCert.empty() && !configClientKey.empty()) {
                config.credentials.authentication.certificate = configClientCert.c_str();
                config.credentials.authentication.key = configClientKey.c_str();
                LOGTV(MQTT, "Client cert:\n%s",
                    config.credentials.authentication.certificate);
            }
        }
    }

private:
    static constexpr milliseconds MQTT_NETWORK_TIMEOUT = 15s;
    static constexpr milliseconds MQTT_MESSAGE_RETRANSMIT_TIMEOUT = 5s;
    static constexpr milliseconds MQTT_CONNECTION_TIMEOUT = MQTT_NETWORK_TIMEOUT;
    static constexpr milliseconds MQTT_SESSION_KEEP_ALIVE = 120s;
    static constexpr milliseconds MQTT_LOOP_INTERVAL = 1s;
    static constexpr milliseconds MQTT_QUEUE_TIMEOUT = 1s;

    struct PendingSubscription {
        const int messageId;
        const steady_clock::time_point subscribedAt;
    };

    struct OutgoingMessage {
        const std::string topic;
        const std::string payload;
        const Retention retain;
        const QoS qos;
        TaskHandle_t waitingTask;
        const LogPublish log;
    };

    struct IncomingMessage {
        const std::string topic;
        const std::string payload;
    };

    struct Subscription {
        const std::string topic;
        const QoS qos;
        const SubscriptionHandler handle;
    };

    struct MessagePublished {
        const int messageId;
        const bool success;
    };

    struct Subscribed {
        const int messageId;
    };

    struct Connected {
        const bool sessionPresent;
    };

    struct Disconnected { };

    PublishStatus publish(const std::string& topic, const JsonDocument& json, Retention retain, QoS qos, ticks timeout = MQTT_NETWORK_TIMEOUT, LogPublish log = LogPublish::Log) {
        std::string payload;
        serializeJson(json, payload);
        if (log == LogPublish::Log) {
#ifdef DUMP_MQTT
            LOGTD(MQTT, "Queuing topic '%s'%s (qos = %d, timeout = %lld ms): %s",
                topic.c_str(),
                (retain == Retention::Retain ? " (retain)" : ""),
                static_cast<int>(qos),
                duration_cast<milliseconds>(timeout).count(),
                payload.c_str());
#else
            LOGTV(MQTT, "Queuing topic '%s'%s (qos = %d, timeout = %lld ms)",
                topic.c_str(),
                (retain == Retention::Retain ? " (retain)" : ""),
                static_cast<int>(qos),
                duration_cast<milliseconds>(timeout).count());
#endif
        }
        return publishAndWait(topic, payload, retain, qos, timeout);
    }

    PublishStatus clear(const std::string& topic, Retention retain, QoS qos, ticks timeout = MQTT_NETWORK_TIMEOUT) {
        LOGTD(MQTT, "Clearing topic '%s' (qos = %d, timeout = %lld ms)",
            topic.c_str(),
            static_cast<int>(qos),
            duration_cast<milliseconds>(timeout).count());
        return publishAndWait(topic, "", retain, qos, timeout);
    }

    PublishStatus publishAndWait(const std::string& topic, const std::string& payload, Retention retain, QoS qos, ticks timeout) {
        TaskHandle_t waitingTask = timeout == ticks::zero() ? nullptr : xTaskGetCurrentTaskHandle();

        bool offered = eventQueue.offerIn(
            MQTT_QUEUE_TIMEOUT,
            OutgoingMessage {
                .topic = topic,
                .payload = payload,
                .retain = retain,
                .qos = qos,
                .waitingTask = waitingTask,
                .log = LogPublish::Log,
            });

        if (!offered) {
            return PublishStatus::QueueFull;
        }
        if (waitingTask == nullptr) {
            return PublishStatus::Pending;
        }

        // Wait for task notification
        auto status = static_cast<PublishStatus>(ulTaskNotifyTake(pdTRUE, timeout.count()));
        switch (status) {
            case PublishStatus::TimeOut:
                pendingMessages.cancelWaitingOn(waitingTask);
                return PublishStatus::TimeOut;
            case PublishStatus::Success:
                return PublishStatus::Success;
            default:
                return PublishStatus::Failed;
        }
    }

    bool subscribe(const std::string& topic, QoS qos, SubscriptionHandler handler) {
        return eventQueue.offerIn(
            MQTT_QUEUE_TIMEOUT,
            // TODO Add an actual timeout
            Subscription {
                .topic = topic,
                .qos = qos,
                .handle = std::move(handler),
            });
    }

    static std::string joinStrings(const std::list<std::string>& strings) {
        if (strings.empty()) {
            return "";
        }
        std::string result;
        for (const auto& str : strings) {
            result += str + "\n";
        }
        return result;
    }

    enum class MqttState : uint8_t {
        Disconnected,
        Connecting,
        Connected,
    };

    void runEventLoop(Task& _task) {
        // We are not yet connected
        auto state = MqttState::Disconnected;
        auto connectionStarted = steady_clock::time_point();

        // The first session is always clean
        auto nextSessionShouldBeClean = true;

        // List of messages we are waiting on
        std::list<PendingSubscription> pendingSubscriptions;

        while (true) {
            auto now = steady_clock::now();

            // Cull pending subscriptions
            // TODO Do this with deleted messages?
            pendingSubscriptions.remove_if([&](const auto& pendingSubscription) {
                if (now - pendingSubscription.subscribedAt > MQTT_NETWORK_TIMEOUT) {
                    LOGTE(MQTT, "Subscription timed out with message id %d", pendingSubscription.messageId);
                    // Force next session to start clean, so we can re-subscribe
                    nextSessionShouldBeClean = true;
                    return true;
                }
                return false;
            });

            switch (state) {
                case MqttState::Disconnected:
                    connect(nextSessionShouldBeClean);
                    state = MqttState::Connecting;
                    connectionStarted = now;
                    disconnectCount++;
                    break;
                case MqttState::Connecting:
                    if (now - connectionStarted > MQTT_CONNECTION_TIMEOUT) {
                        LOGTE(MQTT, "Connecting to MQTT server timed out");
                        ready.clear();
                        disconnect();
                        state = MqttState::Disconnected;
                    }
                    break;
                case MqttState::Connected:
                    // Stay connected
                    break;
            }

            eventQueue.drainIn(duration_cast<ticks>(MQTT_LOOP_INTERVAL), [&](const auto& event) {
                std::visit(
                    [&](auto&& arg) {
                        using T = std::decay_t<decltype(arg)>;
                        if constexpr (std::is_same_v<T, Connected>) {
                            LOGTV(MQTT, "Processing connected event, session present: %d",
                                arg.sessionPresent);
                            state = MqttState::Connected;

                            // TODO Should make it work with persistent sessions, but apparently it doesn't
                            // // Next connection can start with a persistent session
                            // nextSessionShouldBeClean = false;

                            if (!arg.sessionPresent) {
                                // Re-subscribe to existing subscriptions
                                // because we got a clean session
                                processSubscriptions(subscriptions, pendingSubscriptions);
                            }
                        } else if constexpr (std::is_same_v<T, Disconnected>) {
                            LOGTV(MQTT, "Processing disconnected event");
                            state = MqttState::Disconnected;
                            stopClient();

                            // Clear pending messages and notify waiting tasks
                            pendingMessages.clear();

                            // Clear pending subscriptions
                            pendingSubscriptions.clear();
                        } else if constexpr (std::is_same_v<T, MessagePublished>) {
                            LOGTV(MQTT, "Processing message published: %d", arg.messageId);
                            pendingMessages.handlePublished(arg.messageId, arg.success);
                        } else if constexpr (std::is_same_v<T, Subscribed>) {
                            LOGTV(MQTT, "Processing subscribed event: %d", arg.messageId);
                            pendingSubscriptions.remove_if([&](const auto& pendingSubscription) {
                                return pendingSubscription.messageId == arg.messageId;
                            });
                        } else if constexpr (std::is_same_v<T, OutgoingMessage>) {
                            LOGTV(MQTT, "Processing outgoing message to %s",
                                arg.topic.c_str());
                            processOutgoingMessage(arg);
                        } else if constexpr (std::is_same_v<T, Subscription>) {
                            LOGTV(MQTT, "Processing subscription");
                            subscriptions.push_back(arg);
                            if (state == MqttState::Connected) {
                                // If we are connected, we need to subscribe immediately.
                                processSubscriptions({ arg }, pendingSubscriptions);
                            } else {
                                // If we are not connected, we need to rely on the next
                                // clean session to make the subscription.
                                nextSessionShouldBeClean = true;
                            }
                        }
                    },
                    event);
            });
        }
    }

    void connect(bool startCleanSession) {
        networkReady.awaitSet();

        stopClient();

        esp_mqtt_client_config_t mqttConfig {};
        configMqttClient(mqttConfig);
        mqttConfig.session.disable_clean_session = !startCleanSession;
        esp_mqtt_set_config(client, &mqttConfig);
        LOGTI(MQTT, "Connecting to %s:%" PRIu32 ", clean session: %d",
            mqttConfig.broker.address.hostname, mqttConfig.broker.address.port, startCleanSession);
        ESP_ERROR_CHECK(esp_mqtt_client_start(client));
        clientRunning = true;
    }

    void disconnect() {
        ready.clear();
        LOGTD(MQTT, "Disconnecting from MQTT server");
        ESP_ERROR_CHECK(esp_mqtt_client_disconnect(client));
        stopClient();
    }

    void stopClient() {
        if (clientRunning) {
            ESP_ERROR_CHECK(esp_mqtt_client_stop(client));
            clientRunning = false;
        }
    }

    bool clientRunning = false;

    static void handleMqttEventCallback(void* userData, esp_event_base_t _eventBase, int32_t eventId, void* eventData) {
        auto* event = static_cast<esp_mqtt_event_handle_t>(eventData);
        // LOGTV(MQTT, "Event dispatched from event loop: base=%s, event_id=%d, client=%p, data=%p, data_len=%d, topic=%p, topic_len=%d, msg_id=%d",
        //     eventBase, event->event_id, event->client, event->data, event->data_len, event->topic, event->topic_len, event->msg_id);
        auto* driver = static_cast<MqttDriver*>(userData);
        driver->handleMqttEvent(eventId, event);
    }

    void handleMqttEvent(int eventId, esp_mqtt_event_handle_t event) {
        switch (eventId) {
            case MQTT_EVENT_BEFORE_CONNECT: {
                LOGTD(MQTT, "Connecting to MQTT server %s:%" PRIu32, hostname.c_str(), port);
                break;
            }
            case MQTT_EVENT_CONNECTED: {
                LOGTD(MQTT, "Connected to MQTT server");
                ready.set();
                eventQueue.offerIn(MQTT_QUEUE_TIMEOUT, Connected { static_cast<bool>(event->session_present) });
                break;
            }
            case MQTT_EVENT_DISCONNECTED: {
                LOGTD(MQTT, "Disconnected from MQTT server");
                ready.clear();
                eventQueue.offerIn(MQTT_QUEUE_TIMEOUT, Disconnected {});
                break;
            }
            case MQTT_EVENT_SUBSCRIBED: {
                LOGTV(MQTT, "Subscribed, message ID: %d", event->msg_id);
                eventQueue.offerIn(MQTT_QUEUE_TIMEOUT, Subscribed { event->msg_id });
                break;
            }
            case MQTT_EVENT_UNSUBSCRIBED: {
                LOGTV(MQTT, "Unsubscribed, message ID: %d", event->msg_id);
                break;
            }
            case MQTT_EVENT_PUBLISHED: {
                LOGTV(MQTT, "Published, message ID %d", event->msg_id);
                eventQueue.offerIn(MQTT_QUEUE_TIMEOUT, MessagePublished { .messageId = event->msg_id, .success = true });
                break;
            }
            case MQTT_EVENT_DELETED: {
                LOGTV(MQTT, "Deleted, message ID %d", event->msg_id);
                eventQueue.offerIn(MQTT_QUEUE_TIMEOUT, MessagePublished { .messageId = event->msg_id, .success = false });
                break;
            }
            case MQTT_EVENT_DATA: {
                std::string topic(event->topic, event->topic_len);
                std::string payload(event->data, event->data_len);
                LOGTV(MQTT, "Received message on topic '%s'",
                    topic.c_str());
                incomingQueue.offerIn(MQTT_QUEUE_TIMEOUT, IncomingMessage { .topic = topic, .payload = payload });
                break;
            }
            case MQTT_EVENT_ERROR: {
                switch (event->error_handle->error_type) {
                    case MQTT_ERROR_TYPE_TCP_TRANSPORT:
                        LOGTE(MQTT, "TCP transport error; esp_transport_sock_errno: %d, esp_tls_last_esp_err: 0x%x, esp_tls_stack_err: 0x%x, esp_tls_cert_verify_flags: 0x%x",
                            event->error_handle->esp_transport_sock_errno,
                            event->error_handle->esp_tls_last_esp_err,
                            event->error_handle->esp_tls_stack_err,
                            event->error_handle->esp_tls_cert_verify_flags);
                        break;

                    case MQTT_ERROR_TYPE_CONNECTION_REFUSED:
                        LOGTE(MQTT, "Connection refused; return code: %d",
                            event->error_handle->connect_return_code);
                        break;

                    case MQTT_ERROR_TYPE_SUBSCRIBE_FAILED:
                        LOGTE(MQTT, "Subscribe failed; message ID: %d",
                            event->msg_id);
                        break;

                    case MQTT_ERROR_TYPE_NONE:
                        // Nothing to report
                        break;
                }
                if (event->msg_id != 0) {
                    eventQueue.offerIn(MQTT_QUEUE_TIMEOUT, MessagePublished { .messageId = event->msg_id, .success = false });
                }
                break;
            }
            default: {
                LOGTW(MQTT, "Unknown event %d", eventId);
                break;
            }
        }
    }

    void processOutgoingMessage(const OutgoingMessage& message) {
        int ret = esp_mqtt_client_enqueue(
            client,
            message.topic.c_str(),
            message.payload.c_str(),
            static_cast<int>(message.payload.length()),
            static_cast<int>(message.qos),
            static_cast<int>(message.retain == Retention::Retain),
            true);

        if (ret < 0) {
            LOGTD(MQTT, "Error publishing to '%s': %s",
                message.topic.c_str(), ret == -2 ? "outbox full" : "failure");
            PendingMessages::notifyWaitingTask(message.waitingTask, false);
        } else {
            auto messageId = ret;
#ifdef DUMP_MQTT
            if (message.log == LogPublish::Log) {
                LOGTV(MQTT, "Published to '%s' (size: %d), message ID: %d",
                    message.topic.c_str(), message.payload.length(), messageId);
            }
#endif
            pendingMessages.waitOn(messageId, message.waitingTask);
        }
    }

    void processSubscriptions(const std::list<Subscription>& subscriptions, std::list<PendingSubscription>& pendingSubscriptions) {
        std::vector<esp_mqtt_topic_t> topics;
        for (auto it = subscriptions.begin(); it != subscriptions.end();) {
            // Break up subscriptions into batches
            for (; it != subscriptions.end() && topics.size() < 8; it++) {
                const auto& subscription = *it;
                LOGTV(MQTT, "Subscribing to topic '%s' (qos = %d)",
                    subscription.topic.c_str(), static_cast<int>(subscription.qos));
                topics.emplace_back(subscription.topic.c_str(), static_cast<int>(subscription.qos));
            }

            processSubscriptionBatch(topics, pendingSubscriptions);
            topics.clear();
        }
    }

    void processSubscriptionBatch(const std::vector<esp_mqtt_topic_t>& topics, std::list<PendingSubscription>& pendingSubscriptions) {
        int ret = esp_mqtt_client_subscribe_multiple(client, topics.data(), static_cast<int>(topics.size()));

        if (ret < 0) {
            LOGTD(MQTT, "Error subscribing: %s",
                ret == -2 ? "outbox full" : "failure");
        } else {
            auto messageId = ret;
            LOGTV(MQTT, "%d subscriptions published, message ID = %d",
                topics.size(), messageId);
            if (messageId > 0) {
                // Record pending task
                pendingSubscriptions.emplace_back(messageId, steady_clock::now());
            }
        }
    }

    void processIncomingMessage(const IncomingMessage& message) {
        const std::string& topic = message.topic;
        const std::string& payload = message.payload;

        if (payload.empty()) {
            LOGTV(MQTT, "Ignoring empty payload");
            return;
        }

#ifdef DUMP_MQTT
        LOGTD(MQTT, "Received '%s' (size: %d): %s",
            topic.c_str(), payload.length(), payload.c_str());
#else
        LOGTD(MQTT, "Received '%s' (size: %d)",
            topic.c_str(), payload.length());
#endif
        for (const auto& subscription : subscriptions) {
            if (topicMatches(subscription.topic.c_str(), topic.c_str())) {
                Task::run("mqtt:incoming-handler", 4096, [topic, payload, subscription](Task& _task) {
                    JsonDocument json;
                    deserializeJson(json, payload);
                    subscription.handle(topic, json.as<JsonObject>());
                });
                return;
            }
        }
        LOGTW(MQTT, "No handler for topic '%s'",
            topic.c_str());
    }

    static std::string getClientId(const std::string& clientId, const std::string& instanceName) {
        if (!clientId.empty()) {
            return clientId;
        }
        return "ugly-duckling-" + instanceName;
    }

    static bool topicMatches(const char* pattern, const char* topic) {
        const char* pat_ptr = pattern;
        const char* top_ptr = topic;

        while ((*pat_ptr != 0) && (*top_ptr != 0)) {
            // Extract pattern level
            const char* pat_end = strchr(pat_ptr, '/');
            size_t pat_len = (pat_end != nullptr) ? static_cast<size_t>(pat_end - pat_ptr) : strlen(pat_ptr);

            // Extract topic level
            const char* top_end = strchr(top_ptr, '/');
            size_t top_len = (top_end != nullptr) ? static_cast<size_t>(top_end - top_ptr) : strlen(top_ptr);

            // Handle wildcard +
            if (strncmp(pat_ptr, "+", pat_len) == 0) {
                // Match any single level, so just advance
            } else if (strncmp(pat_ptr, "#", pat_len) == 0) {
                // # must be at the end of the pattern
                return *(pat_ptr + pat_len) == '\0';
            } else {
                // Compare level literally
                if (pat_len != top_len || strncmp(pat_ptr, top_ptr, pat_len) != 0) {
                    return false;
                }
            }

            // Move to next level
            if (pat_end != nullptr) {
                pat_ptr = pat_end + 1;
            } else {
                pat_ptr += pat_len;
            }

            if (top_end != nullptr) {
                top_ptr = top_end + 1;
            } else {
                top_ptr += top_len;
            }
        }

        // Handle cases like pattern: "foo/#", topic: "foo"
        if (*pat_ptr == '#' && *(pat_ptr + 1) == '\0') {
            return true;
        }

        return *pat_ptr == '\0' && *top_ptr == '\0';
    }

    State& networkReady;

    const std::string configHostname;
    const unsigned int configPort;
    const std::string configServerCert;
    const std::string configClientCert;
    const std::string configClientKey;
    const std::string clientId;

    StateSource& ready;

    std::string hostname;
    uint32_t port {};
    esp_mqtt_client_handle_t client;

    Queue<std::variant<Connected, Disconnected, MessagePublished, Subscribed, OutgoingMessage, Subscription>> eventQueue;
    Queue<IncomingMessage> incomingQueue;
    // TODO Use a map instead
    std::list<Subscription> subscriptions;
    PendingMessages pendingMessages;

    std::atomic<int> disconnectCount { 0 };

    friend class MqttRoot;
};

}    // namespace farmhub::kernel::mqtt
