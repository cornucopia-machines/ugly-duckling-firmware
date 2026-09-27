#pragma once

#include <Log.hpp>

#include <nvs.h>

#include <cstdint>

namespace cornucopia::ugly_duckling::kernel {

/**
 * @brief Counts every boot of the device in NVS, so the count survives power cycles.
 *
 * The count identifies the boot a log record belongs to (`session` in the `log` payload, and
 * `bootCount` in BOOT). The server never compares counts to order boots -- it orders them by
 * reception time -- so the count only has to tell boots apart. That is why a reset of the count
 * (an NVS erase, a factory reset, a reflash) is harmless. It has to live in NVS, though: RAM-backed
 * counters are lost on a power cycle (and `RTC_DATA_ATTR` ones even on every reset other than a
 * deep-sleep wake, as the bootloader reloads them from the image), which would reuse counts all
 * the time.
 *
 * A consecutive count also makes lost boots visible: a server that sees 41 and then 43 knows it
 * never received anything from boot 42.
 */
class BootCounter {
public:
    /**
     * @brief Increments the persisted boot count and returns the count for this boot (1 on the
     * first boot). Call once, early at boot, after NVS is initialized.
     *
     * Returns 0, which no real boot uses, if NVS can't be opened at all. If only the write fails,
     * this boot's count is still returned, and the next boot reuses it, which is rare enough not
     * to matter.
     */
    static uint32_t recordBoot() {
        nvs_handle_t handle;
        esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            LOGE("Failed to open NVS for boot counter: %s", esp_err_to_name(err));
            return 0;
        }

        uint32_t previousCount = 0;
        err = nvs_get_u32(handle, KEY, &previousCount);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            LOGE("Failed to read boot counter: %s", esp_err_to_name(err));
        }
        uint32_t count = previousCount + 1;

        err = nvs_set_u32(handle, KEY, count);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        if (err != ESP_OK) {
            LOGE("Failed to store boot counter: %s", esp_err_to_name(err));
        }
        nvs_close(handle);
        return count;
    }

private:
    static constexpr const char* NAMESPACE = "boot";
    static constexpr const char* KEY = "count";
};

}    // namespace cornucopia::ugly_duckling::kernel
