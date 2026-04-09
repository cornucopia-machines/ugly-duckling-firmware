#pragma once

#include <string>

#include <ArduinoJson.h>

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>

#include <Log.hpp>
#include <NvsStore.hpp>
#include <Watchdog.hpp>
#include <drivers/WiFiDriver.hpp>
#include <utility>

namespace cornucopia::ugly_duckling::kernel {

LOGGING_TAG(UPDATE, "update")

class HttpUpdater {
public:
    static void startUpdate(const std::string& url, const std::shared_ptr<NvsStore>& nvs) {
        nvs->set(HttpUpdater::UPDATE_KEY, url);
        Task::run("update", 3072, [](Task& _task) {
            LOGTI(UPDATE, "Restarting in 5 seconds to apply update");
            Task::delay(5s);
            esp_restart();
        });
    }

    static void performPendingHttpUpdateIfNecessary(const std::shared_ptr<NvsStore>& nvs, const std::shared_ptr<WiFiDriver>& wifi, std::shared_ptr<Watchdog> watchdog) {
        // Do we need to update?
        if (!nvs->contains(UPDATE_KEY)) {
            LOGTV(UPDATE, "No pending update found, not updating");
            return;
        }

        std::string url;
        if (!nvs->get<std::string>(UPDATE_KEY, url) || url.empty()) {
            LOGTE(UPDATE, "Failed to read pending update URL");
            nvs->remove(UPDATE_KEY);
            return;
        }

        if (!nvs->remove(UPDATE_KEY)) {
            LOGTE(UPDATE, "Failed to delete pending update key");
            return;
        }

        HttpUpdater updater(std::move(watchdog));
        updater.performPendingHttpUpdate(url, wifi);
    }

    static constexpr const char* UPDATE_KEY = "pending-update";

private:
    HttpUpdater(std::shared_ptr<Watchdog> watchdog)
        : watchdog(std::move(watchdog)) {
    }

    void performPendingHttpUpdate(const std::string& url, const std::shared_ptr<WiFiDriver>& wifi) {
        LOGTI(UPDATE, "Updating from version %s via URL %s",
            firmwareVersion, url.c_str());

        LOGTD(UPDATE, "Waiting for network...");
        if (!wifi->getNetworkReady().awaitSet(15s)) {
            LOGTE(UPDATE, "Network not ready, aborting update");
            return;
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

        esp_err_t ret = esp_https_ota(&otaConfig);
        if (ret == ESP_OK) {
            LOGTI(UPDATE, "Update succeeded, rebooting in 5 seconds...");
            Task::delay(5s);
            esp_restart();
        } else {
            LOGTE(UPDATE, "Update failed (%s), continuing with regular boot",
                esp_err_to_name(ret));
            return;
        }
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
            case HTTP_EVENT_ON_DATA: {
                LOGTD(UPDATE, "HTTP data: %d bytes", event->data_len);
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

    const std::shared_ptr<Watchdog> watchdog;
    size_t downloaded = 0;

    static constexpr const size_t DOWNLOAD_NOTIFICATION_BATCH = 128 * 1024;
};

}    // namespace cornucopia::ugly_duckling::kernel
