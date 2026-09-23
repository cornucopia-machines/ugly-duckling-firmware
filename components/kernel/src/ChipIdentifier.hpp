#pragma once

#include <esp_chip_info.h>
#include <sdkconfig.h>

#include <string>

namespace cornucopia::ugly_duckling::kernel {

/**
 * @brief Returns the MCU model and silicon revision, e.g. "esp32c6 v0.1".
 *
 * Silicon errata are specified per chip model and revision, so this lets crash reports be
 * matched against them (see #643).
 */
inline std::string getChipIdentifier() {
    esp_chip_info_t info;
    esp_chip_info(&info);
    // esp_chip_info() encodes the revision as major * 100 + minor
    return std::string(CONFIG_IDF_TARGET) + " (model " + std::to_string(info.model) + ") v" + std::to_string(info.revision / 100) + "." + std::to_string(info.revision % 100);
}

}    // namespace cornucopia::ugly_duckling::kernel
