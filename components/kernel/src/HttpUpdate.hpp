#pragma once
#include <Log.hpp>
#include <NvsStore.hpp>
#include <PowerManager.hpp>
#include <RamCertBundle.hpp>
#include <Restart.hpp>
#include <State.hpp>
#include <Task.hpp>
#include <Watchdog.hpp>
#include <config/ConfigState.hpp>
#include <mqtt/TlsTransport.hpp>

#include <ArduinoJson.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_transport.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace cornucopia::ugly_duckling::kernel {

LOGGING_TAG(UPDATE, "update")

class HttpUpdater {
public:
    /**
     * @brief The link the image is downloaded over.
     */
    struct Network {
        const State& ready;
        // Waited on (for a while) before downloading, so that MQTT's TLS handshake isn't running
        // alongside ours; nullptr when MQTT isn't started during the update
        const State* mqttReady = nullptr;
        // The cellular modem's socket transport to download over; nullptr for lwIP's own
        esp_transport_handle_t modemTransport = nullptr;
    };

    static void startUpdate(const std::string& url, const std::shared_ptr<NvsStore>& nvs) {
        nvs->set(HttpUpdater::UPDATE_KEY, url);
        Task::run("update", 3072, [](Task& _task) {
            LOGTI(UPDATE, "Restarting to apply update");
            delayedRestart();
        });
    }

    static std::optional<config::RejectionCode> performPendingHttpUpdateIfNecessary(const std::shared_ptr<NvsStore>& nvs, const Network& network, std::shared_ptr<Watchdog> watchdog, const std::string& firmwareVersion) {
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
        updater.performPendingHttpUpdate(url, network);
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
    [[noreturn]] void performPendingHttpUpdate(const std::string& url, const Network& network) {
        LOGTI(UPDATE, "Updating from version %s via URL %s",
            firmwareVersion.c_str(), url.c_str());

        // Mark that an update is being attempted. Unless the update succeeds, this marker
        // survives the reboot -- whether we crashed or failed cleanly -- and triggers a
        // rejection on the next boot so the server stops retrying.
        nvs->set(UPDATE_FAILED_KEY, url);

        // Full speed and no light sleep until the restart: over the modem, the data has to be read
        // out of its 2 KB receive buffer as fast as it arrives, or it's lost, and at the lowest
        // CPU frequency, decrypting and decoding it is slower. Nothing else runs during the update
        PowerManagementLock cpuFrequencyMax("update:cpu", ESP_PM_CPU_FREQ_MAX);
        PowerManagementLockGuard fullSpeed(cpuFrequencyMax);
        PowerManagementLock noLightSleep("update:awake", ESP_PM_NO_LIGHT_SLEEP);
        PowerManagementLockGuard awake(noLightSleep);

        bool overModem = network.modemTransport != nullptr;

        LOGTD(UPDATE, "Waiting for network...");
        if (!network.ready.awaitSet(overModem ? MODEM_NETWORK_READY_TIMEOUT : NETWORK_READY_TIMEOUT)) {
            LOGTE(UPDATE, "Network not ready, aborting update, restarting...");
            delayedRestart();
        }

        // Let MQTT finish connecting first: two concurrent TLS handshakes (plus BLE) can exhaust
        // internal RAM on ESP32-C6. Proceed without MQTT if it can't connect, though; an
        // unreachable broker should not block the update.
        if (network.mqttReady != nullptr) {
            LOGTD(UPDATE, "Waiting for MQTT...");
            if (!network.mqttReady->awaitSet(15s)) {
                LOGTW(UPDATE, "MQTT not ready, updating without it");
            }
        }

        // Over the modem, the HTTP client runs on its socket transport, with TLS on top for HTTPS.
        // Lives until the restart below, as the HTTP client only borrows it
        std::unique_ptr<mqtt::TlsTransport> modemTls;
        esp_transport_handle_t transport = nullptr;
        if (overModem) {
            transport = network.modemTransport;
            if (url.starts_with("https://")) {
                // The image host is verified against the CA bundle, without a client certificate
                modemTls = std::make_unique<mqtt::TlsTransport>(network.modemTransport, std::nullopt);
                transport = modemTls->getHandle();
            }
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
        if (overModem) {
            httpConfig.transport = transport;
            // How long a read may wait: NB-IoT round trips take seconds, more in poor coverage
            httpConfig.timeout_ms = static_cast<int>(duration_cast<milliseconds>(MODEM_HTTP_TIMEOUT).count());
        }

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
     * @brief Downloads and installs the image, picking up where it left off when the download
     * breaks.
     *
     * Over the modem, received data is lost whenever its 2 KB buffer overflows, and the TLS
     * record it belonged to fails to verify (docs/specs/NB-IoT.md, "`QISEND` / `QIRD` size
     * limits"). Records that fail never reach the image, so everything written so far is good,
     * and a fresh connection asks for the rest with a Range request. If the server ignores the
     * Range, esp_https_ota starts over from the beginning. Gives up after a few attempts in a
     * row that didn't get any further.
     */
    esp_err_t runOta(esp_https_ota_config_t otaConfig) {
        LOGTI(UPDATE, "Attempting OTA update from URL %s",
            otaConfig.http_config->url);

        size_t written = 0;
        int attemptsWithoutProgress = 0;
        while (true) {
            size_t writtenBefore = written;
            esp_err_t err = attemptOta(otaConfig, written);
            if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
                return err;
            }
            attemptsWithoutProgress = written > writtenBefore ? 0 : attemptsWithoutProgress + 1;
            if (attemptsWithoutProgress >= MAX_ATTEMPTS_WITHOUT_PROGRESS) {
                LOGTE(UPDATE, "Download broke off %d times without getting further, giving up",
                    attemptsWithoutProgress);
                return ESP_FAIL;
            }
            Task::delay(RETRY_DELAY);
            otaConfig.ota_resumption = true;
            otaConfig.ota_image_bytes_written = written;
            LOGTI(UPDATE, "Download broke off, resuming after %.02f KB", static_cast<double>(written) / 1024.0);
        }
    }

    /**
     * @brief Equivalent of esp_https_ota(), but only holds the RAM copy of the CA bundle while
     * connecting.
     *
     * The bundle is only needed for the TLS handshake(s) in esp_https_ota_begin(), which also
     * follows redirects; freeing it before the download gives its RAM back while WiFi buffers
     * the incoming image.
     *
     * @param written how much of the image is in flash, updated when the download breaks off
     * @return ESP_ERR_HTTPS_OTA_IN_PROGRESS when the download broke off and can be resumed
     */
    esp_err_t attemptOta(const esp_https_ota_config_t& otaConfig, size_t& written) {
        statusCode = 0;
        esp_https_ota_handle_t handle = nullptr;
        esp_err_t err;
        {
            RamCertBundle ramCertBundle;
            err = esp_https_ota_begin(&otaConfig, &handle);
        }
        if (err != ESP_OK) {
            LOGTE(UPDATE, "Could not start the download (%s, HTTP status %d)", esp_err_to_name(err), statusCode);
            // Connecting, the TLS handshake and the request can break off over NB-IoT like the
            // download itself, before any response; a server that answered with an error
            // (a wrong URL, say) won't do better the next time
            bool retry = written > 0 || statusCode == 0;
            return retry ? ESP_ERR_HTTPS_OTA_IN_PROGRESS : err;
        }
        if (handle == nullptr) {
            return ESP_FAIL;
        }

        err = esp_https_ota_perform(handle);
        while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            err = esp_https_ota_perform(handle);
        }

        if (err != ESP_OK) {
            // How much made it to flash; -1 if the download hadn't started writing
            int length = esp_https_ota_get_image_len_read(handle);
            if (length > 0) {
                written = static_cast<size_t>(length);
            }
            esp_https_ota_abort(handle);
            LOGTW(UPDATE, "Download failed (%s)", esp_err_to_name(err));
            return ESP_ERR_HTTPS_OTA_IN_PROGRESS;
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
                statusCode = *reinterpret_cast<int*>(event->data);
                LOGTV(UPDATE, "HTTP status code: %d", statusCode);
                break;
            case HTTP_EVENT_ON_DATA: {
                LOGTV(UPDATE, "HTTP data: %d bytes", event->data_len);
                // Keep running while we are receiving data
                watchdog->restart();
                auto beforeBatch = downloaded / DOWNLOAD_NOTIFICATION_BATCH;
                downloaded += static_cast<size_t>(event->data_len);
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
    // Of the current attempt's response; 0 until one arrives
    int statusCode = 0;

    static constexpr const size_t DOWNLOAD_NOTIFICATION_BATCH = 128 * 1024;

    static constexpr std::chrono::milliseconds NETWORK_READY_TIMEOUT = std::chrono::seconds(15);
    // Registering on a cell can take minutes, especially from a cold start
    static constexpr std::chrono::milliseconds MODEM_NETWORK_READY_TIMEOUT = std::chrono::minutes(5);
    // The same as MQTT's network timeout over the modem
    static constexpr std::chrono::milliseconds MODEM_HTTP_TIMEOUT = std::chrono::seconds(30);
    // A download that breaks off is resumed, unless this many attempts in a row got no further
    static constexpr int MAX_ATTEMPTS_WITHOUT_PROGRESS = 3;
    static constexpr std::chrono::milliseconds RETRY_DELAY = std::chrono::seconds(2);
};

}    // namespace cornucopia::ugly_duckling::kernel
