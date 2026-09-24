#pragma once
#include <Log.hpp>
#include <NvsStore.hpp>
#include <RamCertBundle.hpp>
#include <Restart.hpp>
#include <State.hpp>
#include <Watchdog.hpp>
#include <config/ConfigState.hpp>
#include <drivers/WiFiDriver.hpp>

#include <ArduinoJson.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace cornucopia::ugly_duckling::kernel {

LOGGING_TAG(UPDATE, "update")

class HttpUpdater {
public:
    static void startUpdate(const std::string& url, const std::shared_ptr<NvsStore>& nvs) {
        nvs->set(HttpUpdater::UPDATE_KEY, url);
        Task::run("update", 3072, [](Task& _task) {
            LOGTI(UPDATE, "Restarting to apply update");
            delayedRestart();
        });
    }

    static std::optional<config::RejectionCode> performPendingHttpUpdateIfNecessary(const std::shared_ptr<NvsStore>& nvs, const std::shared_ptr<WiFiDriver>& wifi, const State& mqttReady, std::shared_ptr<Watchdog> watchdog, const std::string& firmwareVersion) {
        // If a previous update attempt failed or crashed (marker survived the reboot),
        // report the failure so the server knows not to re-send the same update.
        if (nvs->contains(UPDATE_FAILED_KEY)) {
            nvs->remove(UPDATE_FAILED_KEY);
            LOGTE(UPDATE, "Previous firmware update failed, rejecting");
            return config::RejectionCode::Internal;
        }

        // Do we need to update?
        if (!nvs->contains(UPDATE_KEY)) {
            LOGTV(UPDATE, "No pending update found, not updating");
            return std::nullopt;
        }

        std::string url;
        if (!nvs->get<std::string>(UPDATE_KEY, url) || url.empty()) {
            LOGTE(UPDATE, "Failed to read pending update URL");
            nvs->remove(UPDATE_KEY);
            return config::RejectionCode::Internal;
        }

        if (!nvs->remove(UPDATE_KEY)) {
            LOGTE(UPDATE, "Failed to delete pending update key");
            return config::RejectionCode::Internal;
        }

        HttpUpdater updater(nvs, std::move(watchdog), firmwareVersion);
        updater.performPendingHttpUpdate(url, wifi, mqttReady);
    }

    /**
     * @brief Whether an update will be attempted during this boot.
     *
     * Lets startup skip optional subsystems (BLE) to leave RAM for the OTA: the device reboots
     * after the attempt either way, so they come back on the next boot.
     */
    static bool isUpdatePending(const std::shared_ptr<NvsStore>& nvs) {
        return nvs->contains(UPDATE_KEY);
    }

    static constexpr const char* UPDATE_KEY = "pending-update";
    static constexpr const char* UPDATE_FAILED_KEY = "update-failed";

private:
    HttpUpdater(const std::shared_ptr<NvsStore>& nvs, std::shared_ptr<Watchdog> watchdog, const std::string& firmwareVersion)
        : nvs(nvs)
        , watchdog(std::move(watchdog))
        , firmwareVersion(firmwareVersion) {
    }

    /**
     * @brief Attempts the update, then reboots regardless of the outcome.
     *
     * Startup skips BLE while an update is pending (see isUpdatePending()), so rebooting after
     * a failure too brings the device back up fully. The next boot reports the failure via the
     * UPDATE_FAILED_KEY marker.
     */
    [[noreturn]] void performPendingHttpUpdate(const std::string& url, const std::shared_ptr<WiFiDriver>& wifi, const State& mqttReady) {
        LOGTI(UPDATE, "Updating from version %s via URL %s",
            firmwareVersion.c_str(), url.c_str());

        // Mark that an update is being attempted. Unless the update succeeds, this marker
        // survives the reboot -- whether we crashed or failed cleanly -- and triggers a
        // rejection on the next boot so the server stops retrying.
        nvs->set(UPDATE_FAILED_KEY, url);

        LOGTD(UPDATE, "Waiting for network...");
        if (!wifi->getNetworkReady().awaitSet(15s)) {
            LOGTE(UPDATE, "Network not ready, aborting update, restarting...");
            delayedRestart();
        }

        // Let MQTT finish connecting first: two concurrent TLS handshakes (plus BLE) can exhaust
        // internal RAM on ESP32-C6. Proceed without MQTT if it can't connect, though; an
        // unreachable broker should not block the update.
        LOGTD(UPDATE, "Waiting for MQTT...");
        if (!mqttReady.awaitSet(15s)) {
            LOGTW(UPDATE, "MQTT not ready, updating without it");
        }

        esp_http_client_config_t httpConfig = {};
        httpConfig.url = url.c_str();
        httpConfig.event_handler = httpEventHandler;
        // Additional buffers to fit headers. The TX buffer holds the request line, so it must
        // fit the URL -- including redirect targets like GitHub's release asset links. Kept
        // small: internal RAM is tight while MQTT's TLS session is also up.
        httpConfig.buffer_size = 4 * 1024;
        httpConfig.buffer_size_tx = 2 * 1024;
        httpConfig.user_data = this;
        httpConfig.crt_bundle_attach = esp_crt_bundle_attach;
        httpConfig.keep_alive_enable = true;

        esp_https_ota_config_t otaConfig = {};
        otaConfig.http_config = &httpConfig;

        esp_err_t ret = runOta(otaConfig);
        if (ret == ESP_OK) {
            nvs->remove(UPDATE_FAILED_KEY);
            LOGTI(UPDATE, "Update succeeded, restarting...");
        } else {
            LOGTE(UPDATE, "Update failed (%s), restarting...", esp_err_to_name(ret));
        }
        delayedRestart();
    }

    /**
     * @brief Equivalent of esp_https_ota(), but only holds the RAM copy of the CA bundle while
     * connecting.
     *
     * The bundle is only needed for the TLS handshake(s) in esp_https_ota_begin(), which also
     * follows redirects; freeing it before the download gives its RAM back while WiFi buffers
     * the incoming image.
     */
    static esp_err_t runOta(const esp_https_ota_config_t& otaConfig) {
        LOGTI(UPDATE, "Attempting OTA update from URL %s",
            otaConfig.http_config->url);

        esp_https_ota_handle_t handle = nullptr;
        esp_err_t err;
        {
            RamCertBundle ramCertBundle;
            err = esp_https_ota_begin(&otaConfig, &handle);
        }
        if (err != ESP_OK) {
            return err;
        }
        if (handle == nullptr) {
            return ESP_FAIL;
        }

        err = esp_https_ota_perform(handle);
        while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            err = esp_https_ota_perform(handle);
        }

        if (err != ESP_OK) {
            esp_https_ota_abort(handle);
            return err;
        }
        return esp_https_ota_finish(handle);
    }

    static esp_err_t httpEventHandler(esp_http_client_event_t* event) {
        auto* updater = static_cast<HttpUpdater*>(event->user_data);
        return updater->handleEvent(event);
    }

    esp_err_t handleEvent(esp_http_client_event_t* event) {
        switch (event->event_id) {
            case HTTP_EVENT_ERROR:
                LOGTE(UPDATE, "HTTP error, status code: %d",
                    esp_http_client_get_status_code(event->client));
                break;
            case HTTP_EVENT_ON_CONNECTED:
                LOGTD(UPDATE, "HTTP connected");
                break;
            case HTTP_EVENT_HEADERS_SENT:
                LOGTV(UPDATE, "HTTP headers sent");
                break;
            case HTTP_EVENT_ON_HEADER:
                LOGTV(UPDATE, "HTTP header: %s: %s", event->header_key, event->header_value);
                break;
            case HTTP_EVENT_ON_HEADERS_COMPLETE:
                LOGTV(UPDATE, "HTTP headers complete");
                break;
            case HTTP_EVENT_ON_STATUS_CODE:
                LOGTV(UPDATE, "HTTP status code: %d", *reinterpret_cast<int*>(event->data));
                break;
            case HTTP_EVENT_ON_DATA: {
                LOGTV(UPDATE, "HTTP data: %d bytes", event->data_len);
                // Keep running while we are receiving data
                watchdog->restart();
                auto beforeBatch = downloaded / DOWNLOAD_NOTIFICATION_BATCH;
                downloaded += event->data_len;
                auto afterBatch = downloaded / DOWNLOAD_NOTIFICATION_BATCH;
                if (beforeBatch < afterBatch) {
                    LOGTI(UPDATE, "Downloaded %.02f KB", ((double) downloaded / 1024.0));
                }
                break;
            }
            case HTTP_EVENT_ON_FINISH:
                LOGTD(UPDATE, "HTTP finished");
                break;
            case HTTP_EVENT_DISCONNECTED:
                LOGTD(UPDATE, "HTTP disconnected");
                break;
            default:
                LOGTW(UPDATE, "Unknown HTTP event %d", event->event_id);
                break;
        }
        return ESP_OK;
    }

    const std::shared_ptr<NvsStore> nvs;
    const std::shared_ptr<Watchdog> watchdog;
    const std::string firmwareVersion;
    size_t downloaded = 0;

    static constexpr const size_t DOWNLOAD_NOTIFICATION_BATCH = 128 * 1024;
};

}    // namespace cornucopia::ugly_duckling::kernel
