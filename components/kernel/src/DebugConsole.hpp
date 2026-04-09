#pragma once

#include <chrono>

#include <esp_private/esp_clk.h>

#include <BatteryManager.hpp>
#include <Strings.hpp>
#include <drivers/RtcDriver.hpp>
#include <drivers/WiFiDriver.hpp>

using namespace std::chrono;

using namespace cornucopia::ugly_duckling::kernel::drivers;

namespace cornucopia::ugly_duckling::kernel {

#ifdef UD_DEBUG
class DebugConsole {
public:
    DebugConsole(const std::shared_ptr<BatteryManager>& battery, const std::shared_ptr<WiFiDriver>& wifi)
        : battery(battery)
        , wifi(wifi) {
        status.reserve(256);
        Task::loop("console", 3072, 1, [this](Task& task) {
            printStatus();
            task.delayUntilAtLeast(250ms);
        });
    }

private:
    void printStatus() {
        static const char* spinner = "|/-\\";
        static const size_t spinnerLength = strlen(spinner);
        auto uptime = duration_cast<milliseconds>(steady_clock::now().time_since_epoch());

        counter = (counter + 1) % spinnerLength;
        status.clear();
        status += "[" + std::string(1, spinner[counter]) + "] ";
        status += "\033[33m" + std::string(firmwareVersion) + "\033[0m";
        status += ", uptime: \033[33m" + toStringWithPrecision(static_cast<double>(uptime.count()) / 1000.0, 1) + "\033[0m s";
        status += ", WIFI: " + std::string(wifiStatus());
        status += ", RTC \033[33m" + std::string(RtcDriver::isTimeSet() ? "OK" : "UNSYNCED") + "\033[0m";
        status += ", heap \033[33m" + toStringWithPrecision(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024.0, 2) + "\033[0m kB";
        status += ", CPU: \033[33m" + std::to_string(esp_clk_cpu_freq() / 1000000) + "\033[0m MHz";

        if (battery != nullptr) {
            status += ", battery: \033[33m" + toStringWithPrecision(battery->getVoltage() / 1000.0, 2) + "\033[0m V";
        }

        printf("\033[1G\033[0K%s", status.c_str());
        (void) fflush(stdout);
        fsync(fileno(stdout));
    }

    static const char* wifiStatus() {
        auto* netif = esp_netif_get_default_netif();
        if (netif == nullptr) {
            return "\033[0;33moff\033[0m";
        }

        wifi_mode_t mode = WIFI_MODE_NULL;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_get_mode(&mode));

        switch (mode) {
            case WIFI_MODE_STA:
                break;
            case WIFI_MODE_NULL:
                return "\033[0;33mNULL\033[0m";
            case WIFI_MODE_AP:
                return "\033[0;32mAP\033[0m";
            case WIFI_MODE_APSTA:
                return "\033[0;32mAPSTA\033[0m";
            case WIFI_MODE_NAN:
                return "\033[0;32mNAN\033[0m";
            default:
                return "\033[0;31m???\033[0m";
        }

        // Retrieve the current Wi-Fi station connection status
        wifi_ap_record_t ap_info;
        esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
        if (err != ESP_OK) {
            return esp_err_to_name(err);
        }

        // Check IP address
        esp_netif_ip_info_t ip_info;
        err = esp_netif_get_ip_info(netif, &ip_info);
        if (err != ESP_OK) {
            return esp_err_to_name(err);
        }

        if (ip_info.ip.addr != 0) {
            static char ip_str[32];
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
            (void) snprintf(ip_str, sizeof(ip_str), "\033[0;33m" IPSTR "\033[0m", IP2STR(&ip_info.ip));
            return ip_str;
        }
        return "\033[0;33mIP?\033[0m";
    }

    const std::shared_ptr<BatteryManager> battery;
    const std::shared_ptr<WiFiDriver> wifi;

    size_t counter {};
    std::string status;
};
#endif

}    // namespace cornucopia::ugly_duckling::kernel
