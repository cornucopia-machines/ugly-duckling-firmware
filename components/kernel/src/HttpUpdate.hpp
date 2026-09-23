#pragma once
#include <Log.hpp>
#include <NvsStore.hpp>
#include <RamCertBundle.hpp>
#include <Restart.hpp>
#include <Watchdog.hpp>
#include <config/ConfigState.hpp>
#include <drivers/WiFiDriver.hpp>

#include <ArduinoJson.h>
#include <esp_crt_bundle.h>
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

    static std::optional<config::RejectionCode> performPendingHttpUpdateIfNecessary(const std::shared_ptr<NvsStore>& nvs, const std::shared_ptr<WiFiDriver>& wifi, std::shared_ptr<Watchdog> watchdog, const std::string& firmwareVersion) {
        // If a previous update attempt crashed (marker survived the reboot),
        // report the failure so the server knows not to re-send the same update.
        if (nvs->contains(UPDATE_FAILED_KEY)) {
            nvs->remove(UPDATE_FAILED_KEY);
            LOGTE(UPDATE, "Previous firmware update crashed, rejecting");
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
        return updater.performPendingHttpUpdate(url, wifi);
    }

    static constexpr const char* UPDATE_KEY = "pending-update";
    static constexpr const char* UPDATE_FAILED_KEY = "update-failed";

private:
    HttpUpdater(const std::shared_ptr<NvsStore>& nvs, std::shared_ptr<Watchdog> watchdog, const std::string& firmwareVersion)
        : nvs(nvs)
        , watchdog(std::move(watchdog))
        , firmwareVersion(firmwareVersion) {
    }

    std::optional<config::RejectionCode> performPendingHttpUpdate(const std::string& url, const std::shared_ptr<WiFiDriver>& wifi) {
        LOGTI(UPDATE, "Updating from version %s via URL %s",
            firmwareVersion.c_str(), url.c_str());

        LOGTD(UPDATE, "Waiting for network...");
        if (!wifi->getNetworkReady().awaitSet(15s)) {
            LOGTE(UPDATE, "Network not ready, aborting update");
            return config::RejectionCode::Internal;
        }

        esp_http_client_config_t httpConfig = {};
        httpConfig.url = url.c_str();
        httpConfig.event_handler = httpEventHandler;
        // Additional buffers to fit headers
        // Updating directly via GitHub's release links requires these
        httpConfig.buffer_size = 4 * 1024;
        httpConfig.buffer_size_tx = 12 * 1024;
        httpConfig.user_data = this;
        httpConfig.crt_bundle_attach = esp_crt_bundle_attach;
        httpConfig.keep_alive_enable = true;

        esp_https_ota_config_t otaConfig = {};
        otaConfig.http_config = &httpConfig;

        // Mark that an update is being attempted. If the device crashes
        // during esp_https_ota(), this marker survives and triggers a
        // rejection on the next boot so the server stops retrying.
        nvs->set(UPDATE_FAILED_KEY, url);

        esp_err_t ret;
        {
            RamCertBundle ramCertBundle;
            ret = esp_https_ota(&otaConfig);
        }

        // Clear the crash marker — we got here without crashing
        nvs->remove(UPDATE_FAILED_KEY);

        if (ret == ESP_OK) {
            LOGTI(UPDATE, "Update succeeded, restarting...");
            delayedRestart();
            return std::nullopt;
        }
        LOGTE(UPDATE, "Update failed (%s), continuing with regular boot",
            esp_err_to_name(ret));
        return config::RejectionCode::Internal;
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
