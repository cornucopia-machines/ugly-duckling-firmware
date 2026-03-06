#include "PowerManager.hpp"

namespace farmhub::kernel {

PowerManagementLock PowerManager::noLightSleep("no-light-sleep", ESP_PM_NO_LIGHT_SLEEP);

}    // namespace farmhub::kernel
