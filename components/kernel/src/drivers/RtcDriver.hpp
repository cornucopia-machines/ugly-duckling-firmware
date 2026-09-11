#pragma once
#include <State.hpp>
#include <Task.hpp>
#include <config/Configuration.hpp>

#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include <sys/time.h>
#include <time.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace std::chrono;
using namespace std::chrono_literals;

namespace cornucopia::ugly_duckling::kernel::drivers {

LOGGING_TAG(RTC, "rtc")

/**
 * @brief Ensures the real-time clock is properly set up and holds a real time.
 *
 * If the RTC is already set during a previous boot, the in-sync state is signalled straight away.
 * Otherwise a single, long-lived SNTP client is started once the network is up, and a task observes
 * its sync notifications.
 */
class RtcDriver {
public:
    class Config : public ConfigurationSection {
    public:
        Property<std::string> host { this, "host", "" };
    };

    RtcDriver(State& networkReady, const std::shared_ptr<Config>& ntpConfig, StateSource& rtcInSync)
        : configuredServer(ntpConfig->host.get())
        , rtcInSync(rtcInSync) {

        if (isTimeSet()) {
            LOGTI(RTC, "time is already set");
            rtcInSync.set();
        }

        Task::run("ntp-sync", 4096, [this, &networkReady](Task& _task) {
            networkReady.awaitSet();
            startSntp();

            while (true) {
                // The SNTP client stays alive for the lifetime of the device: lwIP keeps polling on
                // its own (with exponential backoff between failed requests), and keeps the resolved
                // server address around between attempts. Tearing the client down and recreating it
                // per attempt -- as we used to -- threw all of that away, and left the device unable
                // to ever acquire time again until it was power-cycled.
                auto ret = esp_netif_sntp_sync_wait(ticks(this->rtcInSync.isSet() ? SYNCED_POLL_INTERVAL : UNSYNCED_POLL_INTERVAL).count());
                switch (ret) {
                    case ESP_OK:
                    case ESP_ERR_NOT_FINISHED:
                        // It's okay to assume RTC is _roughly_ in sync even if
                        // we're not yet finished with smooth sync
                        this->rtcInSync.set();
                        LOGTI(RTC, "Sync finished successfully (0x%x)", ret);
                        break;
                    case ESP_ERR_TIMEOUT:
                        logNoSync();
                        break;
                    default:
                        LOGTW(RTC, "Waiting for NTP sync failed with %s (0x%x)", esp_err_to_name(ret), ret);
                        break;
                }
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

    void setTime(time_t utcTime) {
        struct timeval tv = { .tv_sec = utcTime, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
        rtcInSync.set();
        LOGTI(RTC, "Time set via BLE CTS");
    }

private:
    static constexpr const char* DEFAULT_NTP_SERVER = "pool.ntp.org";

    // How often we surface diagnostics while we have no valid time; once we do have it,
    // we only wake up to observe the periodic re-syncs lwIP performs on its own.
    static constexpr auto UNSYNCED_POLL_INTERVAL = 30s;
    static constexpr auto SYNCED_POLL_INTERVAL = 1h;

    void startSntp() {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(DEFAULT_NTP_SERVER);
        config.start = false;
        config.smooth_sync = true;
        config.server_from_dhcp = true;
        config.renew_servers_after_new_IP = true;
        config.wait_for_sync = true;
        config.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
        config.sync_cb = onTimeSynced;
        ESP_ERROR_CHECK(esp_netif_sntp_init(&config));

        if (!configuredServer.empty()) {
            // Note lwIP stores the server name by pointer without copying it, so this has to
            // reference storage that outlives the SNTP client -- hence the member field.
            esp_sntp_setservername(0, configuredServer.c_str());
        }

        ESP_ERROR_CHECK(esp_netif_sntp_start());
        LOGTI(RTC, "Started SNTP client with server '%s'", serverName(0));
    }

    void logNoSync() {
        // The reachability shift register (RFC 5905) tells apart "requests go out, nothing comes
        // back" from a client that never got as far as asking.
        unsigned int reachability = 0;
        esp_netif_sntp_reachability(0, &reachability);
        if (rtcInSync.isSet()) {
            LOGTD(RTC, "No NTP sync in the last hour (server '%s', reachability 0x%x)",
                serverName(0), reachability);
        } else {
            LOGTW(RTC, "Still no NTP sync (server '%s', reachability 0x%x, clock at %lld)",
                serverName(0), reachability, static_cast<long long>(time(nullptr)));
        }
    }

    static const char* serverName(uint8_t index) {
        const char* name = esp_sntp_getservername(index);
        return name == nullptr ? "<none>" : name;
    }

    // Runs on the lwIP task right after the clock has been updated; the value it reports is the
    // one the server actually sent, which is the only way to tell a garbage response apart from
    // a response that never arrived.
    static void onTimeSynced(struct timeval* tv) {
        LOGTI(RTC, "NTP response applied, server time is %lld",
            tv == nullptr ? -1LL : static_cast<long long>(tv->tv_sec));
    }

    const std::string configuredServer;
    StateSource& rtcInSync;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
