#pragma once

#include <chrono>
#include <optional>
#include <time.h>

#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include <Configuration.hpp>
#include <State.hpp>
#include <Task.hpp>
#include <utility>

using namespace std::chrono;
using namespace std::chrono_literals;

namespace cornucopia::ugly_duckling::kernel::drivers {

LOGGING_TAG(RTC, "rtc")

/**
 * @brief Ensures the real-time clock is properly set up and holds a real time.
 *
 * The driver runs two tasks:
 *
 * - The first task waits for the system time to be set. It sets the RTC in sync state when the time is set.
 *   This task is non-blocking, and will pass if the RTC is already set during a previous boot.
 */
class RtcDriver {
public:
    class Config : public ConfigurationSection {
    public:
        Property<std::string> host { this, "host", "" };
    };

    RtcDriver(State& networkReady, const std::shared_ptr<Config>& ntpConfig, StateSource& rtcInSync)
        : ntpConfig(ntpConfig)
        , rtcInSync(rtcInSync) {

        if (isTimeSet()) {
            LOGTI(RTC, "time is already set");
            rtcInSync.set();
        }

        Task::run("ntp-sync", 4096, [this, &networkReady](Task& _task) {
            while (true) {
                {
                    networkReady.awaitSet();
                    if (!updateTime()) {
                        // Attempt a retry
                        // TODO Do exponential backoff
                        LOGTE(RTC, "NTP update failed, retrying in 10 seconds");
                        Task::delay(10s);
                        continue;
                    }
                }

                // We are good for a while now
                Task::delay(1h);
            }
        });
    }

    static bool isTimeSet() {
        auto now = system_clock::now();
        // This is 2022-01-01 00:00:00 UTC
        const time_point limit = system_clock::from_time_t(1640995200);
        // The MCU boots with a timestamp of 0 seconds, so if the value is
        // much higher, then it means the RTC is set.
        return now > limit;
    }

    State& getInSync() {
        return rtcInSync;
    }

private:
    bool updateTime() {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        config.start = false;
        config.smooth_sync = true;
        config.server_from_dhcp = true;
        config.renew_servers_after_new_IP = true;
        config.wait_for_sync = true;
        config.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
        ESP_ERROR_CHECK(esp_netif_sntp_init(&config));

        if (!ntpConfig->host.get().empty()) {
            LOGTD(RTC, "Using NTP server %s from configuration",
                ntpConfig->host.get().c_str());
            esp_sntp_setservername(0, ntpConfig->host.get().c_str());
        }

        bool success = false;
        ESP_ERROR_CHECK(esp_netif_sntp_start());

        auto ret = esp_netif_sntp_sync_wait(ticks(10s).count());
        // It's okay to assume RTC is _roughly_ in sync even if
        // we're not yet finished with smooth sync
        if (ret == ESP_OK || ret == ESP_ERR_NOT_FINISHED) {
            rtcInSync.set();
            success = true;
            LOGTD(RTC, "Sync finished successfully");
        } else if (ret == ESP_ERR_TIMEOUT) {
            LOGTD(RTC, "Waiting for time sync timed out");
        } else {
            LOGTD(RTC, "Waiting for time sync returned 0x%x", ret);
        }

        esp_netif_sntp_deinit();
        return success;
    }

    const std::shared_ptr<Config> ntpConfig;
    StateSource& rtcInSync;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
