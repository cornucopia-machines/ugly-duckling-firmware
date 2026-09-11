#pragma once
#include <State.hpp>
#include <Task.hpp>
#include <config/Configuration.hpp>

#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "lwip/ip_addr.h"
#include <sys/time.h>
#include <time.h>

#include <chrono>
#include <cstdio>
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
 * If the RTC still holds a valid time from a previous boot (which survives a soft reset, but not a
 * power cycle), the in-sync state is signalled straight away. Otherwise a single, long-lived SNTP
 * client is started once the network is up, and a task observes its sync notifications. The server
 * offered by DHCP is used when there is one, with the configured and public servers behind it.
 *
 * The in-sync state is only ever signalled once the system clock itself holds a plausible
 * wall-clock time -- never merely because an SNTP call reported success.
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

        // Do this before anything else: lwIP only keeps the NTP server offered in a DHCP lease if
        // DHCP server mode is already enabled by the time that lease is processed, and WiFiDriver
        // is already associating from its own task by the time we get here. The client itself is
        // only started once we actually have a network.
        initSntp();

        if (isTimeSet()) {
            markInSync("retained across reboot");
        }

        Task::run("ntp-sync", 4096, [this, &networkReady](Task& _task) {
            networkReady.awaitSet();
            ESP_ERROR_CHECK(esp_netif_sntp_start());
            LOGTI(RTC, "Started SNTP client; servers: %s", describeServers().c_str());

            while (true) {
                // The SNTP client stays alive for the lifetime of the device: lwIP keeps polling
                // on its own (with exponential backoff between failed requests), and keeps the
                // resolved server address around between attempts. Tearing the client down and
                // recreating it per attempt -- as we used to -- threw all of that away, and left
                // the device unable to ever acquire time again until it was power-cycled.
                auto ret = esp_netif_sntp_sync_wait(ticks(this->rtcInSync.isSet() ? SYNCED_POLL_INTERVAL : UNSYNCED_POLL_INTERVAL).count());
                switch (ret) {
                    case ESP_OK:
                    case ESP_ERR_NOT_FINISHED:
                        // ESP_ERR_NOT_FINISHED would mean a smooth sync is still slewing, with the
                        // clock already roughly right. Either way, trust the clock and not the
                        // return code -- a sync notification can also arrive for a response that
                        // left the clock somewhere near the boot epoch.
                        if (isTimeSet()) {
                            markInSync(ret == ESP_OK ? "NTP" : "NTP (smooth sync in progress)");
                        } else {
                            LOGTW(RTC, "NTP sync notification (0x%x) left the clock unset at %lld, ignoring",
                                ret, static_cast<long long>(time(nullptr)));
                        }
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
        // The MCU boots with a timestamp of 0 seconds, so if the value is
        // much higher, then it means the RTC is set.
        return time(nullptr) > EARLIEST_PLAUSIBLE_TIME;
    }

    State& getInSync() {
        return rtcInSync;
    }

    void setTime(time_t utcTime) {
        // Never let a bogus value from the outside move a clock we already trust, and never
        // arm the in-sync state on a value that isn't a plausible wall-clock time.
        if (utcTime <= EARLIEST_PLAUSIBLE_TIME) {
            LOGTW(RTC, "Ignoring implausible time %lld received via BLE CTS",
                static_cast<long long>(utcTime));
            return;
        }
        struct timeval tv = { .tv_sec = utcTime, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
        markInSync("BLE CTS");
    }

private:
    // 2022-01-01 00:00:00 UTC: no time at or below this can be a real wall-clock time.
    static constexpr time_t EARLIEST_PLAUSIBLE_TIME = 1640995200;

    static constexpr const char* DEFAULT_NTP_SERVER = "pool.ntp.org";

    // How often we surface diagnostics while we have no valid time; once we do have it,
    // we only wake up to observe the periodic re-syncs lwIP performs on its own.
    static constexpr auto UNSYNCED_POLL_INTERVAL = 30s;
    static constexpr auto SYNCED_POLL_INTERVAL = 1h;

    void initSntp() {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(DEFAULT_NTP_SERVER);
        config.start = false;
        // Step the clock rather than slewing it. Smooth sync is documented to fall back to
        // settimeofday() beyond a 35 minute delta, but that fallback is broken in IDF 6.1
        // (espressif/esp-idf#19051): adjtime() narrows `tv_sec * 1000000` to a 32-bit long, so a
        // cold boot's ~57 year delta wraps to a small value that slips past the range check meant
        // to reject it. adjtime() then reports success, lwIP concludes it is slewing and never
        // steps the clock, and the slew is itself abandoned because the boot time is still zero --
        // leaving the clock at time-since-boot forever. Revisit once we are on an IDF with the fix.
        config.smooth_sync = false;
        config.wait_for_sync = true;
        config.sync_cb = onTimeSynced;

        // Accept the NTP server offered by DHCP, but never let it become the only one. lwIP's
        // dhcp_set_ntp_servers() writes the DHCP-supplied list starting at index 0 and NULLs out
        // every remaining slot, so our own servers have to be put back afterwards; esp-netif does
        // that on every new lease, restoring them from `index_of_first_server` onwards. Slot 0 is
        // thus left to DHCP (CONFIG_LWIP_DHCP_MAX_NTP_SERVERS is 1) and slots 1+ stay ours.
        config.server_from_dhcp = true;
        config.renew_servers_after_new_IP = true;
        config.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
        config.index_of_first_server = 1;

        if (!configuredServer.empty()) {
            // Note lwIP stores the server name by pointer without copying it, so this has to
            // reference storage that outlives the SNTP client -- hence the member field.
            config.servers[0] = configuredServer.c_str();
            // Keep the public pool as a fallback behind the configured server, instead of
            // replacing it: lwIP moves on to the next server when the one before is unreachable.
            config.servers[1] = DEFAULT_NTP_SERVER;
            config.num_of_servers = 2;
        }

        ESP_ERROR_CHECK(esp_netif_sntp_init(&config));
    }

    void logNoSync() {
        if (rtcInSync.isSet()) {
            LOGTD(RTC, "No NTP sync in the last hour; servers: %s", describeServers().c_str());
        } else {
            // Reachability distinguishes the two failures that look alike from here: nothing
            // answering at all, versus responses arriving that never reached the clock.
            LOGTW(RTC, "RTC still not in sync, clock at %lld; servers: %s",
                static_cast<long long>(time(nullptr)), describeServers().c_str());
        }
    }

    void markInSync(const char* source) {
        auto now = time(nullptr);
        // Initialized because strftime leaves the buffer indeterminate when it doesn't fit
        char buffer[32] = "";
        struct tm timeInfo {};
        gmtime_r(&now, &timeInfo);
        (void) strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeInfo);
        // Log every arming event with the clock value it armed on, so that a device that ends up
        // running on a bogus clock can be traced back to the source that set it.
        bool alreadyInSync = rtcInSync.isSet();
        rtcInSync.set();
        if (alreadyInSync) {
            LOGTD(RTC, "RTC re-synced via %s, time is %s", source, buffer);
        } else {
            LOGTI(RTC, "RTC in sync via %s, time is %s", source, buffer);
        }
    }

    // Every slot with its reachability register (RFC 5905): which servers we actually have, and
    // whether any of them ever answered. Slot 0 is the DHCP-supplied one, and shows up as a bare
    // IP address rather than a name.
    static std::string describeServers() {
        std::string result;
        for (uint8_t index = 0; index < CONFIG_LWIP_SNTP_MAX_SERVERS; index++) {
            unsigned int reachability = 0;
            esp_netif_sntp_reachability(index, &reachability);
            char entry[128];
            (void) snprintf(entry, sizeof(entry), "#%u '%s' (reachability 0x%x)",
                static_cast<unsigned>(index), describeServer(index).c_str(), reachability);
            if (!result.empty()) {
                result += ", ";
            }
            result += entry;
        }
        return result;
    }

    static std::string describeServer(uint8_t index) {
        const char* name = esp_sntp_getservername(index);
        if (name != nullptr) {
            return name;
        }
        const ip_addr_t* addr = esp_sntp_getserver(index);
        if (addr != nullptr && !ip_addr_isany_val(*addr)) {
            // ipaddr_ntoa() would hand back a shared static buffer; the _r form keeps it ours
            char buffer[IPADDR_STRLEN_MAX] = "";
            return ipaddr_ntoa_r(addr, buffer, sizeof(buffer)) == nullptr ? "<invalid>" : buffer;
        }
        return "<none>";
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
