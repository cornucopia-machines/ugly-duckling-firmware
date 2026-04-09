#include "PowerManager.hpp"

#include <esp_pm.h>

namespace cornucopia::ugly_duckling::kernel {

PowerManagementLock PowerManager::noLightSleep("no-light-sleep", ESP_PM_NO_LIGHT_SLEEP);

}    // namespace cornucopia::ugly_duckling::kernel
