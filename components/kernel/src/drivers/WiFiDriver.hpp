#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <variant>
#include <vector>

#include <esp_event.h>
#include <esp_wifi.h>
#include <network_provisioning/manager.h>
#include <network_provisioning/scheme_softap.h>

#include "WifiApRecord.hpp"
#include <ArduinoJson.h>
#include <Concurrent.hpp>
#include <Overloaded.hpp>
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

    void setOnStatusChanged(std::function<void(const std::string&)> callback) {
        onStatusChanged = std::move(callback);
    }

    // Validates {ssid, password} and triggers a reconnect via the event queue.
    // Validation errors are reported via the status callback as "failed:<reason>".
    void setCredentials(const std::string& ssid, const std::string& password) {
        eventQueue.offer(EvCredentials { .ssid = ssid, .password = password });
    }

    // Drops the current connection; the driver will reconnect immediately.
    void disconnect() {
        eventQueue.offer(EvDisconnectCmd { });
    }

    // Turns off WiFi; the driver will not reconnect until new credentials are set.
    void disable() {
        eventQueue.offer(EvDisableCmd { });
    }

    // Starts a WiFi scan and calls onComplete with the discovered APs when done.
    // If a scan is already in progress the request is ignored.
    void startWifiScan(std::function<void(std::vector<WifiApRecord>)> onComplete) {
        eventQueue.offer(EvScanRequest { std::move(onComplete) });
    }

    void populateTelemetry(JsonObject& json) {
        if (networkReady.isSet()) {
            wifi_ap_record_t apInfo = { };
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
    // Only called from runLoop() — no mutex needed.
    void setWifiStatusInternal(const std::string& newStatus) {
        if (wifiStatus != newStatus) {
            wifiStatus = newStatus;
            if (onStatusChanged) {
                onStatusChanged(newStatus);
            }
        }
    }

    static uint8_t wifiGenFromRecord(const wifi_ap_record_t& r) {
        if (r.phy_11ax) { return 6;
}
        if (r.phy_11n) { return 4;
}
        if (r.phy_11g) { return 3;
}
        return 1;
    }

    static const char* wifiAuthModeStr(wifi_auth_mode_t mode) {
        switch (mode) {
            case WIFI_AUTH_OPEN:
                return "open";
            case WIFI_AUTH_WEP:
                return "wep";
            case WIFI_AUTH_WPA_PSK:
                return "wpa_psk";
            case WIFI_AUTH_WPA2_PSK:
                return "wpa2_psk";
            case WIFI_AUTH_WPA_WPA2_PSK:
                return "wpa_wpa2_psk";
            case WIFI_AUTH_WPA3_PSK:
                return "wpa3_psk";
            case WIFI_AUTH_WPA2_WPA3_PSK:
                return "wpa2_wpa3_psk";
            default:
                return "unknown";
        }
    }

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
            case WIFI_EVENT_SCAN_DONE: {
                // network_prov_mgr may trigger its own scans (SoftAP provisioning
                // AP list); in that case don't consume results — it must read
                // them from the WiFi stack itself.
                if (scanTriggeredByProvisioning.exchange(true)) {
                    break;
                }
                uint16_t num = 0;
                esp_wifi_scan_get_ap_num(&num);
                std::vector<wifi_ap_record_t> raw(num);
                esp_wifi_scan_get_ap_records(&num, raw.data());

                std::vector<WifiApRecord> aps;
                aps.reserve(num);
                for (uint16_t i = 0; i < num; i++) {
                    aps.push_back({
                        .ssid = reinterpret_cast<const char*>(raw[i].ssid),
                        .rssi = raw[i].rssi,
                        .authMode = wifiAuthModeStr(raw[i].authmode),
                        .wifiGen = wifiGenFromRecord(raw[i]),
                    });
                }
                LOGTD(WIFI, "WiFi scan done: %d APs found", num);
                eventQueue.offer(EvScanDone { std::move(aps) });
                break;
            }
            case WIFI_EVENT_STA_START: {
                LOGTD(WIFI, "Started");
                stationStarted.set();
                eventQueue.offer(EvStarted { });
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
                eventQueue.offer(EvDisconnected { event->reason });
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
                eventQueue.offer(EvGotIp { });
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
                eventQueue.offer(EvLostIp { });
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
                eventQueue.offer(EvProvisioningDone { });
                network_prov_mgr_deinit();
                break;
            }
            default:
                break;
        }
    }

    void runLoop() {
        bool disabled = false;
        bool connected = false;
        steady_clock::time_point connectingSince;
        std::optional<std::function<void(std::vector<WifiApRecord>)>> pendingScanCallback;
        while (true) {
            if (!connected) {
                if (disabled) {
                    // nothing — just drain events
                } else if (configPortalRunning.isSet()) {
                    // TODO Add some sort of timeout here
                    LOGTV(WIFI, "Provisioning already running");
                } else if (networkConnecting.isSet()) {
                    if (steady_clock::now() - connectingSince < WIFI_CONNECTION_TIMEOUT) {
                        LOGTV(WIFI, "Already connecting");
                    } else {
                        LOGTI(WIFI, "Connection timed out, retrying");
                        networkConnecting.clear();
                        ensureWifiStopped();
                        connectingSince = steady_clock::now();
                        connect();
                    }
                } else {
                    connectingSince = steady_clock::now();
                    connect();
                }
            }

            eventQueue.drainIn(duration_cast<ticks>(WIFI_CHECK_INTERVAL), [&](const auto& event) {
                std::visit(
                    overloaded {
                        [&](const EvStarted&) {
                            if (!configPortalRunning.isSet()) {
                                esp_err_t err = esp_wifi_connect();
                                if (err != ESP_OK) {
                                    LOGTD(WIFI, "Failed to start connecting: %s, stopping", esp_err_to_name(err));
                                    ensureWifiStopped();
                                }
                            }
                        },
                        [&](const EvGotIp&) {
                            connected = true;
                            networkConnecting.clear();
                            setWifiStatusInternal("connected");
                            LOGTD(WIFI, "Connected to the network");
                        },
                        [&](const EvLostIp&) {
                            connected = false;
                            networkConnecting.clear();
                            setWifiStatusInternal("connecting");
                            disconnectCount++;
                        },
                        [&](const EvDisconnected& ev) {
                            connected = false;
                            networkConnecting.clear();
                            if (!disabled) {
                                if (ev.reason == WIFI_REASON_AUTH_FAIL
                                    || ev.reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
                                    || ev.reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
                                    setWifiStatusInternal("failed:auth_failed");
                                } else if (ev.reason == WIFI_REASON_NO_AP_FOUND) {
                                    setWifiStatusInternal("failed:ap_not_found");
                                } else {
                                    setWifiStatusInternal("connecting");
                                }
                            }
                            disconnectCount++;
                            LOGTD(WIFI, "Disconnected from the network");
                        },
                        [&](const EvProvisioningDone&) {
                            configPortalRunning.clear();
                        },
                        [&](const EvScanDone& ev) {
                            if (pendingScanCallback.has_value()) {
                                auto cb = std::move(*pendingScanCallback);
                                pendingScanCallback.reset();
                                cb(ev.records);
                            }
                        },
                        [&](const EvScanRequest& ev) {
                            if (pendingScanCallback.has_value()) {
                                return;    // scan already in progress, ignore
                            }
                            pendingScanCallback = ev.onComplete;
                            wifi_mode_t mode = WIFI_MODE_NULL;
                            esp_wifi_get_mode(&mode);
                            if (mode == WIFI_MODE_NULL) {
                                auto cb = std::move(*pendingScanCallback);
                                pendingScanCallback.reset();
                                cb({});
                                return;
                            }
                            wifi_scan_config_t scanConfig = { };
                            scanTriggeredByProvisioning = false;
                            esp_err_t err = esp_wifi_scan_start(&scanConfig, false);
                            if (err != ESP_OK) {
                                LOGTD(WIFI, "Failed to start WiFi scan: %s", esp_err_to_name(err));
                                scanTriggeredByProvisioning = true;
                                auto cb = std::move(*pendingScanCallback);
                                pendingScanCallback.reset();
                                cb({});
                            }
                        },
                        [&](const EvCredentials& ev) {
                            if (ev.ssid.size() > sizeof(wifi_sta_config_t::ssid) - 1) {
                                setWifiStatusInternal("failed:ssid_too_long");
                                return;
                            }
                            if (ev.password.size() > sizeof(wifi_sta_config_t::password) - 1) {
                                setWifiStatusInternal("failed:password_too_long");
                                return;
                            }
                            wifi_config_t config = { };
                            strncpy(reinterpret_cast<char*>(config.sta.ssid), ev.ssid.c_str(), sizeof(config.sta.ssid) - 1);
                            strncpy(reinterpret_cast<char*>(config.sta.password), ev.password.c_str(), sizeof(config.sta.password) - 1);
                            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
                            setWifiStatusInternal("connecting");
                            connected = false;
                            disabled = false;
                            networkConnecting.clear();
                            configPortalRunning.clear();
                            ensureWifiStopped();
                        },
                        [&](const EvDisconnectCmd&) {
                            ensureWifiStopped();
                        },
                        [&](const EvDisableCmd&) {
                            connected = false;
                            networkConnecting.clear();
                            disabled = true;
                            setWifiStatusInternal("disabled");
                            ensureWifiStopped();
                        },
                    },
                    event);
            });
        }
    }

    void connect() {
        networkConnecting.set();

#ifdef WOKWI
        LOGTD(WIFI, "Skipping provisioning on Wokwi");
        wifi_config_t wifiConfig = { };
        strncpy(reinterpret_cast<char*>(wifiConfig.sta.ssid), "Wokwi-GUEST", sizeof(wifiConfig.sta.ssid) - 1);
        wifiConfig.sta.ssid[sizeof(wifiConfig.sta.ssid) - 1] = '\0';
        wifiConfig.sta.password[0] = '\0';
        wifiConfig.sta.channel = 6;
        setWifiStatusInternal("connecting");
        connectToStation(wifiConfig);
#else
        bool provisioned = false;
        ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));
        if (provisioned) {
            wifi_config_t wifiConfig;
            ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &wifiConfig));
            LOGTI(WIFI, "Connecting using stored credentials to %s",
                wifiConfig.sta.ssid);
            setWifiStatusInternal("connecting");
            connectToStation(wifiConfig);
        } else {
            LOGTI(WIFI, "No stored credentials, starting provisioning");
            setWifiStatusInternal("unconfigured");
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

    // Only accessed from runLoop() — no mutex needed.
    std::string wifiStatus { "unconfigured" };
    std::function<void(const std::string&)> onStatusChanged;

    StateSource& networkConnecting;
    StateSource& networkReady;
    StateSource& configPortalRunning;
    const std::string hostname;

    StateManager internalStates;
    StateSource stationStarted = internalStates.createStateSource("wifi:station-started");

    struct EvStarted { };
    struct EvGotIp { };
    struct EvLostIp { };
    struct EvDisconnected {
        uint8_t reason;
    };
    struct EvProvisioningDone { };
    struct EvScanDone {
        std::vector<WifiApRecord> records;
    };
    struct EvScanRequest {
        std::function<void(std::vector<WifiApRecord>)> onComplete;
    };
    struct EvCredentials {
        std::string ssid;
        std::string password;
    };
    struct EvDisconnectCmd { };
    struct EvDisableCmd { };
    using WiFiEvent = std::variant<
        EvStarted, EvGotIp, EvLostIp, EvDisconnected, EvProvisioningDone,
        EvScanDone, EvScanRequest, EvCredentials, EvDisconnectCmd, EvDisableCmd>;

    Queue<WiFiEvent> eventQueue { "wifi-events", 16 };

    static constexpr milliseconds WIFI_QUEUE_TIMEOUT = 1s;
    static constexpr milliseconds WIFI_CONNECTION_TIMEOUT = 1min;
    static constexpr milliseconds WIFI_CHECK_INTERVAL = 1min;

    Mutex metadataMutex;
    std::optional<std::string> ssid;
    std::optional<esp_ip4_addr_t> ip;

    std::atomic<int> disconnectCount { 0 };
    // True by default (assumed provisioning-owned); cleared to false when we
    // call esp_wifi_scan_start() so WIFI_EVENT_SCAN_DONE knows to consume the
    // results, then reset to true via exchange.
    std::atomic<bool> scanTriggeredByProvisioning { true };
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
