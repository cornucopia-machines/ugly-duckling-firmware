#pragma once
#include <Log.hpp>
#include <Overloaded.hpp>
#include <Queue.hpp>
#include <State.hpp>
#include <Task.hpp>
#include <config/Configuration.hpp>
#include <mqtt/TlsTransport.hpp>

#include <esp_event.h>
#include <esp_transport.h>
#include <esp_transport_ws.h>
#include <mqtt_client.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono;
using namespace std::chrono_literals;
using namespace cornucopia::ugly_duckling::kernel;

// Forward-declare the drivers namespace so the using directive works regardless of include order
namespace cornucopia::ugly_duckling::kernel::drivers {
}    // namespace cornucopia::ugly_duckling::kernel::drivers
using namespace cornucopia::ugly_duckling::kernel::drivers;

namespace cornucopia::ugly_duckling::kernel::mqtt {

LOGGING_TAG(MQTT, "mqtt")

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
        // Preferred: a full URI like "mqtts://broker.example.com:8883"
        // that encodes scheme, host, port, and path in one string.
        Property<std::string> url { this, "url", "" };
        // Deprecated: use "url" instead. Kept for backward compatibility;
        // ignored when "url" is set.
        Property<std::string> host { this, "host", "" };
        Property<unsigned int> port { this, "port", 1883 };
        Property<size_t> queueSize { this, "queueSize", 128 };
        ArrayProperty<std::string> serverCert { this, "serverCert" };
        ArrayProperty<std::string> clientCert { this, "clientCert" };
        ArrayProperty<std::string> clientKey { this, "clientKey" };
    };

    /**
     * @param modemTransport the cellular modem's socket transport in cellular builds, to connect
     * over instead of lwIP; nullptr for WiFi
     */
    MqttDriver(
        State& networkReady,
        const std::shared_ptr<Config>& config,
        const std::string& clientId,
        StateSource& ready,
        esp_transport_handle_t modemTransport = nullptr)
        : networkReady(networkReady)
        , modemTransport(modemTransport)
        , url(config->url.get())
        , configHostname(config->host.get())
        , configPort(config->port.get())
        , configServerCert(joinStrings(config->serverCert.get()))
        , configClientCert(joinStrings(config->clientCert.get()))
        , configClientKey(joinStrings(config->clientKey.get()))
        , clientId(clientId)
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

    /**
     * @brief Registers a callback fired on every successful (re)connection, including the first.
     * Runs on the MQTT event-loop task, from the Connected event -- it must not publish inline
     * (publish() enqueues onto this same task and waits), only hand off work elsewhere (e.g. an
     * overwrite queue picked up by a dedicated task; see the SYNC trigger in Device.hpp). Blocks
     * (rather than dropping the registration on a full queue) since callers register this once at
     * startup and would otherwise boot with no SYNC trigger at all.
     */
    void onConnected(std::function<void()> callback) {
        eventQueue.put(ConnectedListenerRegistration { std::move(callback) });
    }

    void populateTelemetry(JsonObject& json) {
        json["disconnects"] = disconnectCount.exchange(0, std::memory_order_relaxed);
    }

    void configMqttClient(esp_mqtt_client_config_t& config) {
        if (url.empty()) {
            if (!configHostname.empty()) {
                // Legacy host/port mode (deprecated)
                hostname = configHostname;
                port = configPort;
            } else {
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
            }
        }

        // Computed once, used for all log messages
        serverAddress = url.empty()
            ? hostname + ":" + std::to_string(port)
            : url;

        config = {
            .broker {
                .address {
                    .uri = url.empty() ? nullptr : url.c_str(),
                    .hostname = url.empty() ? hostname.c_str() : nullptr,
                    .transport = MQTT_TRANSPORT_OVER_TCP,
                    .path = nullptr,
                    .port = url.empty() ? port : 0,
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
                .keepalive = static_cast<int>(duration_cast<seconds>(modemTransport == nullptr ? MQTT_SESSION_KEEP_ALIVE : MODEM_SESSION_KEEP_ALIVE).count()),
                .disable_keepalive = false,
                .protocol_ver = MQTT_PROTOCOL_UNDEFINED,    // Default MQTT version
                .message_retransmit_timeout = static_cast<int>(duration_cast<milliseconds>(modemTransport == nullptr ? MQTT_MESSAGE_RETRANSMIT_TIMEOUT : MODEM_MESSAGE_RETRANSMIT_TIMEOUT).count()),
            },
            .network {
                .reconnect_timeout_ms = duration_cast<milliseconds>(MQTT_CONNECTION_TIMEOUT).count(),
                .timeout_ms = static_cast<int>(duration_cast<milliseconds>(networkTimeout()).count()),
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
            .outbox {
                .limit = MQTT_OUTBOX_LIMIT_BYTES,
            },
        };

        LOGTI(MQTT, "Server: %s, client ID is '%s'",
            serverAddress.c_str(),
            config.credentials.client_id);

        if (modemTransport != nullptr) {
            // esp-mqtt ignores the URI's scheme when given a transport, and takes its TLS settings
            // only for transports it creates itself, so the stack is built here to match
            config.network.transport = getModemTransport();
            if (!url.empty()) {
                // Leave it to the URI, as it is anyway, instead of a warning about the conflict
                config.broker.address.transport = MQTT_TRANSPORT_UNKNOWN;
            }
            // TLS runs on esp-mqtt's task here, with the AT command layer underneath
            config.task.stack_size = MODEM_TASK_STACK_SIZE;
            return;
        }

        if (!configServerCert.empty()) {
            if (url.empty()) {
                // In URI mode the scheme already selects the transport
                config.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
            }
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
    /**
     * @brief The modem's transport with whatever the URI's scheme asks for on top: TLS for
     * mqtts and wss (or a server certificate in legacy host mode), WebSocket for ws and wss.
     * Built on first use, so a bad certificate shows up where connecting does, not at startup.
     */
    esp_transport_handle_t getModemTransport() {
        if (modemTransportStack != nullptr) {
            return modemTransportStack;
        }
        auto schemeEnd = url.find("://");
        std::string scheme = schemeEnd == std::string::npos ? "" : url.substr(0, schemeEnd);
        bool secure = url.empty() ? !configServerCert.empty() : (scheme == "mqtts" || scheme == "wss");
        bool webSocket = scheme == "ws" || scheme == "wss";

        esp_transport_handle_t transport = modemTransport;
        if (secure) {
            if (configServerCert.empty()) {
                throw std::runtime_error("TLS over the modem needs a server certificate in the configuration");
            }
            modemTlsTransport = std::make_unique<TlsTransport>(modemTransport, TlsTransport::Credentials {
                                                                                   .serverCert = configServerCert,
                                                                                   .clientCert = configClientCert,
                                                                                   .clientKey = configClientKey,
                                                                               });
            transport = modemTlsTransport->getHandle();
        }
        if (webSocket) {
            // The same setup esp-mqtt gives the WebSocket transports it creates itself
            esp_transport_handle_t ws = esp_transport_ws_init(transport);
            if (ws == nullptr) {
                throw std::runtime_error("could not create WebSocket transport");
            }
            auto pathStart = url.find('/', schemeEnd + 3);
            if (pathStart != std::string::npos) {
                // Copied by the transport
                esp_transport_ws_set_path(ws, url.substr(pathStart).c_str());
            }
            esp_transport_ws_set_subprotocol(ws, "mqtt");
            esp_transport_set_default_port(ws, secure ? 443 : 80);
            transport = ws;
        }
        modemTransportStack = transport;
        return modemTransportStack;
    }

    // Bounds how long we wait for the *network* to respond: esp-mqtt's transport read/write
    // timeout, the connection attempt, and a subscription ack. Publishing never waits on the
    // network at all, see publish().
    static constexpr milliseconds MQTT_NETWORK_TIMEOUT = 15s;
    // NB-IoT round trips take seconds, more in poor coverage
    static constexpr milliseconds MODEM_NETWORK_TIMEOUT = 30s;

    milliseconds networkTimeout() const {
        return modemTransport == nullptr ? MQTT_NETWORK_TIMEOUT : MODEM_NETWORK_TIMEOUT;
    }

    static constexpr milliseconds MQTT_MESSAGE_RETRANSMIT_TIMEOUT = 5s;
    // esp-mqtt resends an unacknowledged message on the same connection after this long. Over
    // NB-IoT the ack routinely takes longer than 5 s when the link stalls, and every resend is the
    // whole message again over the air: one night in the field had up to six copies of a telemetry
    // message arriving together. TCP already delivers what was sent on a live connection, so
    // resending there buys little; a reconnect requeues what wasn't acknowledged yet anyway.
    static constexpr milliseconds MODEM_MESSAGE_RETRANSMIT_TIMEOUT = 1min;
    // Messages wait in the outbox until acknowledged, or until CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS
    // after they were queued or first sent (10 minutes, to outlast an NB-IoT reconnect: see
    // sdkconfig.defaults). esp-mqtt limits the total size of the queued messages, in bytes, and only
    // if told to: without a limit, a long outage would fill the heap with log records and
    // telemetry. What doesn't fit waits in our own queue (see holdBack()), up to its own limit,
    // so at most the two together stay in RAM
    static constexpr uint64_t MQTT_OUTBOX_LIMIT_BYTES = uint64_t { 16 } * 1024;
    static constexpr size_t MQTT_HELD_BACK_LIMIT_BYTES = 16 * 1024;
    static constexpr milliseconds MQTT_CONNECTION_TIMEOUT = MQTT_NETWORK_TIMEOUT;
    static constexpr milliseconds MQTT_SESSION_KEEP_ALIVE = 120s;
    // esp-mqtt pings at half the keepalive, and every ping costs about 200 bytes over the air
    // with TLS, TCP and their ACKs: at 120 s that would be twice the SIM's daily data budget. It
    // also has to stay under the carrier's NAT idle timeout, which isn't measured yet
    // (docs/specs/NB-IoT.md, "Keepalive and session expiry"); 10 minutes, so pings every 5
    // minutes (about 58 KB a day), until it is.
    static constexpr milliseconds MODEM_SESSION_KEEP_ALIVE = 10min;
    static constexpr uint32_t MODEM_TASK_STACK_SIZE = 8192;
    static constexpr milliseconds MQTT_LOOP_INTERVAL = 1s;
    static constexpr milliseconds MQTT_QUEUE_TIMEOUT = 1s;

    struct PendingSubscription {
        int messageId;
        steady_clock::time_point subscribedAt;
    };

    struct OutgoingMessage {
        std::string topic;
        std::string payload;
        QoS qos;
        LogPublish log;
    };

    struct IncomingMessage {
        std::string topic;
        std::string payload;
    };

    struct Subscription {
        std::string topic;
        QoS qos;
        SubscriptionHandler handle;
    };

    struct Subscribed {
        int messageId;
    };

    struct Connected {
        bool sessionPresent;
    };

    struct Disconnected { };

    struct ConnectedListenerRegistration {
        std::function<void()> callback;
    };

    /**
     * Fire-and-forget: hands the message to the driver task, which enqueues it into esp-mqtt's
     * outbox. Nothing waits for the broker's ack. A wait never affected delivery, and no caller
     * acted on the outcome, so it only delayed the publishing task. MqttLog was the last caller
     * to wait, to keep log records in order, until issue #635 gave them an explicit sequence.
     */
    void publish(const std::string& topic, const JsonDocument& json, QoS qos, LogPublish log = LogPublish::Log) {
        std::string payload;
        serializeJson(json, payload);
        if (log == LogPublish::Log) {
#ifdef DUMP_MQTT
            LOGTD(MQTT, "Queuing topic '%s' (qos = %d): %s",
                topic.c_str(),
                static_cast<int>(qos),
                payload.c_str());
#else
            LOGTV(MQTT, "Queuing topic '%s' (qos = %d)",
                topic.c_str(),
                static_cast<int>(qos));
#endif
        }
        // A full queue is already reported by Queue itself
        eventQueue.offerIn(
            MQTT_QUEUE_TIMEOUT,
            OutgoingMessage {
                .topic = topic,
                .payload = payload,
                .qos = qos,
                .log = log,
            });
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

    static std::string joinStrings(const std::vector<std::string>& strings) {
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
        std::vector<PendingSubscription> pendingSubscriptions;

        // Messages that didn't fit in esp-mqtt's outbox yet, oldest first (see holdBack())
        std::deque<OutgoingMessage> heldBack;
        size_t heldBackBytes = 0;

        while (true) {
            auto now = steady_clock::now();

            // Cull pending subscriptions
            // TODO Do this with deleted messages?
            std::erase_if(pendingSubscriptions, [&](const auto& pendingSubscription) {
                if (now - pendingSubscription.subscribedAt > networkTimeout()) {
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
                    if (now - connectionStarted > networkTimeout()) {
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

            // Acks and expiry make room in the outbox; the oldest held-back messages go first
            while (!heldBack.empty() && processOutgoingMessage(heldBack.front()) != EnqueueResult::OutboxFull) {
                heldBackBytes -= heldBack.front().payload.size();
                heldBack.pop_front();
            }

            eventQueue.drainIn(duration_cast<ticks>(MQTT_LOOP_INTERVAL), [&](const auto& event) {
                std::visit(
                    overloaded {
                        [&](const Connected& arg) {
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

                            for (const auto& listener : connectedListeners) {
                                listener();
                            }
                        },
                        [&](const Disconnected&) {
                            LOGTV(MQTT, "Processing disconnected event");
                            state = MqttState::Disconnected;
                            stopClient();

                            // Clear pending subscriptions
                            pendingSubscriptions.clear();
                        },
                        [&](const Subscribed& arg) {
                            LOGTV(MQTT, "Processing subscribed event: %d", arg.messageId);
                            std::erase_if(pendingSubscriptions, [&](const auto& pendingSubscription) {
                                return pendingSubscription.messageId == arg.messageId;
                            });
                        },
                        [&](const OutgoingMessage& arg) {
                            LOGTV(MQTT, "Processing outgoing message to %s",
                                arg.topic.c_str());
                            // Behind what's held back already, to keep the order
                            if (!heldBack.empty() || processOutgoingMessage(arg) == EnqueueResult::OutboxFull) {
                                holdBack(heldBack, heldBackBytes, arg);
                            }
                        },
                        [&](const Subscription& arg) {
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
                        },
                        [&](const ConnectedListenerRegistration& arg) {
                            LOGTV(MQTT, "Processing connected-listener registration");
                            connectedListeners.push_back(arg.callback);
                        },
                    },
                    event);
            });
        }
    }

    void connect(bool startCleanSession) {
        networkReady.awaitSet();

        stopClient();

        esp_mqtt_client_config_t mqttConfig {};
        try {
            configMqttClient(mqttConfig);
        } catch (const std::exception& e) {
            // Config is loaded once at startup and won't fix itself without a reboot, but we
            // still just leave the state machine to retry on its normal cadence (see the
            // Connecting-state timeout in runEventLoop()) rather than special-casing a backoff --
            // the client was never started, so that retry is as cheap as this one was.
            LOGTE(MQTT, "Cannot configure MQTT client, not connecting: %s", e.what());
            return;
        }
        mqttConfig.session.disable_clean_session = !startCleanSession;
        esp_mqtt_set_config(client, &mqttConfig);
        LOGTI(MQTT, "Connecting to %s, clean session: %d",
            serverAddress.c_str(), startCleanSession);
        ESP_ERROR_CHECK(esp_mqtt_client_start(client));
        clientRunning = true;
    }

    void disconnect() {
        ready.clear();
        // Guard against disconnecting a client that was never started -- e.g. connect() bailed
        // out above because of a config error, so esp_mqtt_client_start() was never called.
        if (clientRunning) {
            LOGTD(MQTT, "Disconnecting from MQTT server");
            ESP_ERROR_CHECK(esp_mqtt_client_disconnect(client));
        }
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
                LOGTD(MQTT, "Connecting to MQTT server %s", serverAddress.c_str());
                connectStartedAt = steady_clock::now();
                break;
            }
            case MQTT_EVENT_CONNECTED: {
                // Socket, TLS and the MQTT CONNECT round trip: what a reconnect costs
                LOGTI(MQTT, "Connected to MQTT server in %lld ms",
                    static_cast<long long>(duration_cast<milliseconds>(steady_clock::now() - connectStartedAt).count()));
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
                break;
            }
            case MQTT_EVENT_DELETED: {
                LOGTV(MQTT, "Deleted, message ID %d", event->msg_id);
                break;
            }
            case MQTT_EVENT_DATA: {
                std::string topic(event->topic, static_cast<size_t>(event->topic_len));
                std::string payload(event->data, static_cast<size_t>(event->data_len));
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
                break;
            }
            default: {
                LOGTW(MQTT, "Unknown event %d", eventId);
                break;
            }
        }
    }

    enum class EnqueueResult : uint8_t {
        Enqueued,
        // Try again once acks or expiry have made room
        OutboxFull,
        Failed,
    };

    EnqueueResult processOutgoingMessage(const OutgoingMessage& message) {
        int ret = esp_mqtt_client_enqueue(
            client,
            message.topic.c_str(),
            message.payload.c_str(),
            static_cast<int>(message.payload.length()),
            static_cast<int>(message.qos),
            0,    // Never retain: nothing the device publishes is a retained message
            true);
        if (ret == -2) {
            return EnqueueResult::OutboxFull;
        }

        // Silent publishes (log records) must not log here: the log line would be published in
        // turn, and a failing publish would then feed itself. A lost log record still shows up as
        // a gap in its `seq`.
        if (message.log == LogPublish::Silent) {
            return ret < 0 ? EnqueueResult::Failed : EnqueueResult::Enqueued;
        }
        if (ret < 0) {
            LOGTD(MQTT, "Error publishing to '%s'", message.topic.c_str());
            return EnqueueResult::Failed;
        }
#ifdef DUMP_MQTT
        LOGTV(MQTT, "Published to '%s' (size: %d), message ID: %d",
            message.topic.c_str(), message.payload.length(), ret);
#endif
        return EnqueueResult::Enqueued;
    }

    /**
     * @brief Keeps a message that doesn't fit in the outbox until it does, instead of dropping it.
     *
     * The outbox fills up while the connection is down, e.g. with the boot's log records until an
     * NB-IoT connection is up, and when the broker's acks come slower than we publish. Held-back
     * messages don't expire, but they are capped too: past MQTT_HELD_BACK_LIMIT_BYTES, new
     * messages are dropped.
     */
    static void holdBack(std::deque<OutgoingMessage>& heldBack, size_t& heldBackBytes, const OutgoingMessage& message) {
        // One that wouldn't fit even in an empty outbox would hold up everything behind it
        if (message.payload.size() > MQTT_OUTBOX_LIMIT_BYTES || heldBackBytes + message.payload.size() > MQTT_HELD_BACK_LIMIT_BYTES) {
            // Silent publishes (log records) must not log: see processOutgoingMessage()
            if (message.log != LogPublish::Silent) {
                LOGTD(MQTT, "Error publishing to '%s': outbox full, and too much held back already", message.topic.c_str());
            }
            return;
        }
        heldBack.push_back(message);
        heldBackBytes += message.payload.size();
    }

    void processSubscriptions(const std::vector<Subscription>& subscriptions, std::vector<PendingSubscription>& pendingSubscriptions) {
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

    void processSubscriptionBatch(const std::vector<esp_mqtt_topic_t>& topics, std::vector<PendingSubscription>& pendingSubscriptions) {
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
                // Handlers can run deep: applying an `update` copies nested config documents, and
                // ArduinoJson's copy recurses once per nesting level. 4 KB overflowed on a device
                // config with peripherals and functions.
                Task::run("mqtt:incoming-handler", 8192, [topic, payload, subscription](Task& _task) {
                    JsonDocument json;
                    deserializeJson(json, payload);
                    subscription.handle(topic, json.as<JsonObject>());
                    // On ESP-IDF, stack sizes and the high-water mark are in bytes
                    LOGTD(MQTT, "Handled '%s', stack high-water mark: %u bytes unused",
                        topic.c_str(), static_cast<unsigned int>(uxTaskGetStackHighWaterMark(nullptr)));
                });
                return;
            }
        }
        LOGTW(MQTT, "No handler for topic '%s'",
            topic.c_str());
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
    esp_transport_handle_t modemTransport;
    std::unique_ptr<TlsTransport> modemTlsTransport;
    // The top of the stack built on modemTransport; esp-mqtt keeps it for good
    esp_transport_handle_t modemTransportStack = nullptr;

    const std::string url;
    const std::string configHostname;
    const unsigned int configPort;
    const std::string configServerCert;
    const std::string configClientCert;
    const std::string configClientKey;
    const std::string clientId;

    StateSource& ready;

    std::string serverAddress;
    std::string hostname;
    uint32_t port {};
    esp_mqtt_client_handle_t client;

    using MqttEvent = std::variant<Connected, Disconnected, Subscribed, OutgoingMessage, Subscription, ConnectedListenerRegistration>;
    Queue<MqttEvent> eventQueue;
    Queue<IncomingMessage> incomingQueue;
    // TODO Use a map instead
    std::vector<Subscription> subscriptions;
    std::vector<std::function<void()>> connectedListeners;

    std::atomic<int> disconnectCount { 0 };

    // Only touched by the event handler, which esp-mqtt calls one event at a time
    steady_clock::time_point connectStartedAt;

    friend class MqttRoot;
};

}    // namespace cornucopia::ugly_duckling::kernel::mqtt
