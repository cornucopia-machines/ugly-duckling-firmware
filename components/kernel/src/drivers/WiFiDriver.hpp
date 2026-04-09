#pragma once

#include <chrono>

#include <esp_event.h>
#include <esp_wifi.h>
#include <network_provisioning/manager.h>
#include <network_provisioning/scheme_softap.h>

#include <Concurrent.hpp>
#include <State.hpp>
#include <StateManager.hpp>
#include <Task.hpp>
#include <Telemetry.hpp>

using namespace std::chrono;
using namespace std::chrono_literals;
using namespace cornucopia::ugly_duckling::kernel;

namespace cornucopia::ugly_duckling::kernel::drivers {

LOGGING_TAG(WIFI, "wifi")

class WiFiDriver final {
public:
    WiFiDriver(
        StateSource& networkConnecting,
        StateSource& networkReady,
        StateSource& configPortalRunning,
        const std::string& hostname)
        : networkConnecting(networkConnecting)
        , networkReady(networkReady)
        , configPortalRunning(configPortalRunning)
        , hostname(hostname) {
        LOGTV(WIFI, "Registering WiFi handlers");

        // Initialize TCP/IP adapter and event loop
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());

        // Create default WiFi station interface
        esp_netif_create_default_wifi_sta();
        esp_netif_create_default_wifi_ap();

        // Register event handlers
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WiFiDriver::onEvent, this));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &WiFiDriver::onEvent, this));
        ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &WiFiDriver::onEvent, this));

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

        Task::run("wifi-driver", 4096, [this](Task&) {
            runLoop();
        });
    }

    static void setPowerSaveMode(bool enable) {
        ESP_ERROR_CHECK(esp_wifi_set_ps(enable
                ? WIFI_PS_MAX_MODEM
                : WIFI_PS_MIN_MODEM));
    }

    std::optional<std::string> getSsid() {
        Lock lock(metadataMutex);
        return ssid;
    }

    std::optional<std::string> getIp() {
        Lock lock(metadataMutex);
        return ip.transform([](const esp_ip4_addr_t& ip) {
            char ipString[16];
            esp_ip4addr_ntoa(&ip, ipString, sizeof(ipString));
            return std::string(ipString);
        });
    }

    void populateTelemetry(JsonObject& json) {
        if (networkReady.isSet()) {
            wifi_ap_record_t apInfo = {};
            esp_err_t err = esp_wifi_sta_get_ap_info(&apInfo);
            if (err == ESP_OK) {
                json["rssi"] = apInfo.rssi;
            } else {
                LOGTD(WIFI, "Failed to get AP info: %s", esp_err_to_name(err));
            }
        }
        json["disconnects"] = disconnectCount.exchange(0, std::memory_order_relaxed);
    }

    State& getNetworkConnecting() {
        return networkConnecting;
    }

    State& getNetworkReady() {
        return networkReady;
    }

    State& getConfigPortalRunning() {
        return configPortalRunning;
    }

private:
    static void onEvent(void* arg, esp_event_base_t eventBase, int32_t eventId, void* eventData) {
        auto* driver = static_cast<WiFiDriver*>(arg);
        if (eventBase == WIFI_EVENT) {
            driver->onWiFiEvent(eventId, eventData);
        } else if (eventBase == IP_EVENT) {
            driver->onIpEvent(eventId, eventData);
        } else if (eventBase == NETWORK_PROV_EVENT) {
            driver->onWiFiProvEvent(eventId, eventData);
        }
    }

    void onWiFiEvent(int32_t eventId, void* eventData) {
        switch (eventId) {
            case WIFI_EVENT_STA_START: {
                LOGTD(WIFI, "Started");
                stationStarted.set();
                eventQueue.offer(WiFiEvent::Started);
                break;
            }
            case WIFI_EVENT_STA_STOP: {
                LOGTD(WIFI, "Stopped");
                stationStarted.clear();
                break;
            }
            case WIFI_EVENT_STA_CONNECTED: {
                auto* event = static_cast<wifi_event_sta_connected_t*>(eventData);
                std::string newSsid(reinterpret_cast<const char*>(event->ssid), event->ssid_len);
                {
                    Lock lock(metadataMutex);
                    ssid = newSsid;
                }
                LOGTD(WIFI, "Connected to the AP %s",
                    newSsid.c_str());
                break;
            }
            case WIFI_EVENT_STA_DISCONNECTED: {
                auto* event = static_cast<wifi_event_sta_disconnected_t*>(eventData);
                networkReady.clear();
                {
                    Lock lock(metadataMutex);
                    ssid.reset();
                }
                eventQueue.offer(WiFiEvent::Disconnected);
                LOGTD(WIFI, "Disconnected from the AP %.*s, reason: %d",
                    event->ssid_len, reinterpret_cast<const char*>(event->ssid), event->reason);
                break;
            }
            case WIFI_EVENT_AP_STACONNECTED: {
                LOGTI(WIFI, "SoftAP transport connected");
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                LOGTI(WIFI, "SoftAP transport disconnected");
                break;
            }
            default:
                break;
        }
    }

    void onIpEvent(int32_t eventId, void* eventData) {
        switch (eventId) {
            case IP_EVENT_STA_GOT_IP: {
                auto* event = static_cast<ip_event_got_ip_t*>(eventData);
                networkReady.set();
                {
                    Lock lock(metadataMutex);
                    ip = event->ip_info.ip;
                }
                eventQueue.offer(WiFiEvent::Connected);
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
                LOGTD(WIFI, "Got IP - " IPSTR, IP2STR(&event->ip_info.ip));
                break;
            }
            case IP_EVENT_STA_LOST_IP: {
                networkReady.clear();
                {
                    Lock lock(metadataMutex);
                    ip.reset();
                }
                eventQueue.offer(WiFiEvent::Disconnected);
                LOGTD(WIFI, "Lost IP");
                break;
            }
            default:
                break;
        }
    }

    void onWiFiProvEvent(int32_t eventId, void* eventData) {
        switch (eventId) {
            case NETWORK_PROV_START: {
                LOGTD(WIFI, "provisioning started");
                break;
            }
            case NETWORK_PROV_WIFI_CRED_RECV: {
                auto* wifiConfig = static_cast<wifi_sta_config_t*>(eventData);
                LOGTD(WIFI, "Received Wi-Fi credentials for SSID '%s'",
                    reinterpret_cast<const char*>(wifiConfig->ssid));
                break;
            }
            case NETWORK_PROV_WIFI_CRED_FAIL: {
                auto* reason = static_cast<network_prov_wifi_sta_fail_reason_t*>(eventData);
                LOGTD(WIFI, "provisioning failed because %s",
                    *reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR
                        ? "authentication failed"
                        : "AP not found");
                ESP_ERROR_CHECK(network_prov_mgr_reset_wifi_sm_state_on_failure());
                break;
            }
            case NETWORK_PROV_WIFI_CRED_SUCCESS: {
                LOGTD(WIFI, "provisioning successful");
                break;
            }
            case NETWORK_PROV_END: {
                LOGTD(WIFI, "provisioning finished");
                eventQueue.offer(WiFiEvent::ProvisioningFinished);
                network_prov_mgr_deinit();
                break;
            }
            default:
                break;
        }
    }

    // TODO Refactor this to avoid using goto
    // NOLINTBEGIN(cppcoreguidelines-avoid-goto)
    void runLoop() {
        bool connected = false;
        steady_clock::time_point connectingSince;
        while (true) {
            if (!connected) {
                if (configPortalRunning.isSet()) {
                    // TODO Add some sort of timeout here
                    LOGTV(WIFI, "Provisioning already running");
                    goto handleEvents;
                }
                if (networkConnecting.isSet()) {
                    if (steady_clock::now() - connectingSince < WIFI_CONNECTION_TIMEOUT) {
                        LOGTV(WIFI, "Already connecting");
                        goto handleEvents;
                    }

                    LOGTI(WIFI, "Connection timed out, retrying");
                    networkConnecting.clear();
                    ensureWifiStopped();
                }
                connectingSince = steady_clock::now();
                connect();
            }

        handleEvents:
            for (auto event = eventQueue.pollIn(WIFI_CHECK_INTERVAL); event.has_value(); event = eventQueue.poll()) {
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
                switch (event.value()) {
                    case WiFiEvent::Started:
                        if (!configPortalRunning.isSet()) {
                            esp_err_t err = esp_wifi_connect();
                            if (err != ESP_OK) {
                                LOGTD(WIFI, "Failed to start connecting: %s, stopping", esp_err_to_name(err));
                                ensureWifiStopped();
                            }
                        }
                        break;
                    case WiFiEvent::Connected:
                        connected = true;
                        networkConnecting.clear();
                        LOGTD(WIFI, "Connected to the network");
                        break;
                    case WiFiEvent::Disconnected:
                        connected = false;
                        networkConnecting.clear();
                        LOGTD(WIFI, "Disconnected from the network");
                        disconnectCount++;
                        break;
                    case WiFiEvent::ProvisioningFinished:
                        configPortalRunning.clear();
                        break;
                }
            }
        }
    }
    // NOLINTEND(cppcoreguidelines-avoid-goto)

    void connect() {
        networkConnecting.set();

#ifdef WOKWI
        LOGTD(WIFI, "Skipping provisioning on Wokwi");
        wifi_config_t wifiConfig = {};
        strncpy(reinterpret_cast<char*>(wifiConfig.sta.ssid), "Wokwi-GUEST", sizeof(wifiConfig.sta.ssid) - 1);
        wifiConfig.sta.ssid[sizeof(wifiConfig.sta.ssid) - 1] = '\0';
        wifiConfig.sta.password[0] = '\0';
        wifiConfig.sta.channel = 6;
        connectToStation(wifiConfig);
#else
        bool provisioned = false;
        ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));
        if (provisioned) {
            wifi_config_t wifiConfig;
            ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &wifiConfig));
            LOGTI(WIFI, "Connecting using stored credentials to %s",
                wifiConfig.sta.ssid);
            connectToStation(wifiConfig);
        } else {
            LOGTI(WIFI, "No stored credentials, starting provisioning");
            configPortalRunning.set();
            startProvisioning();
        }
#endif
    }

    void ensureWifiStationStarted(wifi_config_t& config) {
        if (!stationStarted.isSet()) {
            auto listenInterval = 20;
            LOGTV(WIFI, "Enabling power save mode, listen interval: %d DTIM beacons (%d ms)",
                listenInterval, listenInterval * 100);
            config.sta.listen_interval = listenInterval;
#ifdef SOC_PM_SUPPORT_WIFI_WAKEUP
            LOGTV(WIFI, "Enabling wake on WiFi");
            ESP_ERROR_CHECK(esp_sleep_enable_wifi_wakeup());
#endif
#ifdef SOC_PM_SUPPORT_BEACON_WAKEUP
            LOGTV(WIFI, "Enabling wake on WiFi beacon");
            ESP_ERROR_CHECK(esp_sleep_enable_wifi_beacon_wakeup());
#endif

            LOGTD(WIFI, "Starting station");
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
            ESP_ERROR_CHECK(esp_wifi_start());
            stationStarted.awaitSet();
        }
    }

    void ensureWifiStopped() {
        if (stationStarted.isSet()) {
            if (networkReady.isSet()) {
                ensureWifiDisconnected();
            }
            LOGTD(WIFI, "Stopping");
            // This might return ESP_ERR_WIFI_STOP_STATE, but we can ignore it
            esp_err_t err = esp_wifi_stop();
            if (err != ESP_OK) {
                LOGTD(WIFI, "Failed to stop WiFi: %s, assuming we are still okay", esp_err_to_name(err));
            }
        }
        LOGTD(WIFI, "Stopping WiFi");
    }

    void ensureWifiDisconnected() {
        networkReady.clear();
        if (stationStarted.isSet()) {
            LOGTD(WIFI, "Disconnecting");
            ESP_ERROR_CHECK(esp_wifi_disconnect());
        }
    }

    void connectToStation(wifi_config_t& config) {
        ensureWifiStopped();
        ensureWifiStationStarted(config);
    }

    static void startProvisioning() {
        // Initialize provisioning manager
        network_prov_mgr_config_t config = {
            .scheme = network_prov_scheme_softap,
            .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
            .app_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
            .network_prov_wifi_conn_cfg = {
                // TODO Shall we limit the number of connection attempts?
                .wifi_conn_attempts = 0,    // Infinite attempts
            },
        };
        ESP_ERROR_CHECK(network_prov_mgr_init(config));

        char serviceName[32];
        uint8_t mac[6];
        const char* ssid_prefix = "PROV_";
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        (void) snprintf(serviceName, sizeof(serviceName), "%s%02X%02X%02X",
            ssid_prefix, mac[3], mac[4], mac[5]);
        LOGTD(WIFI, "Starting provisioning service '%s'",
            serviceName);

        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(network_prov_security_t::NETWORK_PROV_SECURITY_1, pop, serviceName, serviceKey));

        // TODO Maybe print QR code?
    }

    static constexpr const char* pop = "abcd1234";
    static constexpr const char* serviceKey = nullptr;
    static constexpr const bool resetProvisioned = false;

    StateSource& networkConnecting;
    StateSource& networkReady;
    StateSource& configPortalRunning;
    const std::string hostname;

    StateManager internalStates;
    StateSource stationStarted = internalStates.createStateSource("wifi:station-started");

    enum class WiFiEvent : uint8_t {
        Started,
        Connected,
        Disconnected,
        ProvisioningFinished,
    };

    CopyQueue<WiFiEvent> eventQueue { "wifi-events", 16 };

    static constexpr milliseconds WIFI_QUEUE_TIMEOUT = 1s;
    static constexpr milliseconds WIFI_CONNECTION_TIMEOUT = 1min;
    static constexpr milliseconds WIFI_CHECK_INTERVAL = 1min;

    Mutex metadataMutex;
    std::optional<std::string> ssid;
    std::optional<esp_ip4_addr_t> ip;

    std::atomic<int> disconnectCount { 0 };
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
