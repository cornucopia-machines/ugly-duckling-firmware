#include "PowerManager.hpp"

#include <esp_pm.h>

namespace farmhub::kernel {

PowerManagementLock PowerManager::noLightSleep("no-light-sleep", ESP_PM_NO_LIGHT_SLEEP);

}    // namespace farmhub::kernel
