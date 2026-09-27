#include "BatteryManager.hpp"
#include "I2CManager.hpp"
#include "KernelStatus.hpp"
#include "NetworkConfig.hpp"
#include "State.hpp"
#include "Task.hpp"
#include "Watchdog.hpp"
#include "devices/DeviceConfiguration.hpp"
#include "devices/DeviceDefinition.hpp"
#include "drivers/BatteryDriver.hpp"
#include "drivers/BleDriver.hpp"
#include "drivers/LedDriver.hpp"
#include <BootHelpers.hpp>
#include <Log.hpp>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"

#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "NvsStore.hpp"
#endif

#include <Restart.hpp>
#include <mqtt/MqttDriver.hpp>

#include <esp_wifi.h>
#include <nvs_flash.h>

#include <chrono>
#include <memory>
#include <string>

using namespace std::chrono;
using namespace cornucopia::ugly_duckling::devices;
using namespace cornucopia::ugly_duckling::kernel;

void performFactoryReset(const std::shared_ptr<LedDriver>& statusLed, bool completeReset) {
    LOGI("Performing factory reset");

    statusLed->turnOn();
    Task::delay(1s);
    statusLed->turnOff();
    Task::delay(1s);
    statusLed->turnOn();

    LOGI(" - Deleting wifi NVS entries...");
    esp_wifi_restore();

    if (completeReset) {
        Task::delay(1s);
        statusLed->turnOff();
        Task::delay(1s);
        statusLed->turnOn();

        LOGI(" - Deleting all NVS config entries...");
        nvs_flash_erase();
    }

    LOGI(" - Restarting...");
    delayedRestart();
}

std::shared_ptr<BatteryDriver> initBattery(const std::shared_ptr<DeviceDefinition>& deviceDefinition, const std::shared_ptr<I2CManager>& i2c) {
    auto battery = deviceDefinition->createBatteryDriver(i2c);
    if (battery != nullptr) {
        // If the battery voltage is below the device's threshold, we should not boot yet.
        // This is to prevent the device from booting and immediately shutting down
        // due to the high current draw of the boot process.
        auto voltage = battery->getVoltage();
        if (voltage != 0 && voltage < battery->parameters.bootThreshold) {
            ESP_LOGW("battery", "Battery voltage too low (%d mV < %d mV), entering deep sleep",
                voltage, battery->parameters.bootThreshold);
            enterLowPowerDeepSleep();
        }
    }
    return battery;
}

void initNvsFlash() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

std::shared_ptr<Watchdog> initWatchdog(seconds timeout) {
    return std::make_shared<Watchdog>("watchdog", timeout, true, [](WatchdogState state) {
        if (state == WatchdogState::TimedOut) {
            LOGE("Watchdog timed out");
            esp_system_abort("Watchdog timed out");
        }
    });
}

std::shared_ptr<MqttRoot> initMqtt(const std::shared_ptr<ModuleStates>& states, const std::string& clientId, const std::shared_ptr<NetworkConfig>& networkConfig, StateSource& mqttReady) {
    // NetworkConfig inherits from MqttDriver::Config, so we can upcast
    auto mqttConfig = std::static_pointer_cast<MqttDriver::Config>(networkConfig);
    auto mqtt = std::make_shared<MqttDriver>(states->networkReady, mqttConfig, clientId, mqttReady);
    return std::make_shared<MqttRoot>(mqtt, networkConfig->getTopicRoot());
}

std::shared_ptr<BleDriver> initBle(
    const std::shared_ptr<DeviceConfiguration>& deviceConfig,
    const std::string& hostname,
    const std::string& deviceDescription,
    const std::string& firmwareVersion,
    const std::string& macAddress) {
#ifdef CONFIG_BT_NIMBLE_ENABLED
    if (deviceConfig->bleEnabled.get()) {
        LOGI("BLE enabled, starting NimBLE stack");
        auto bleNvs = std::make_shared<NvsStore>("ble");
        return std::make_shared<NimBleDriver>(
            hostname,
            deviceDescription,
            firmwareVersion,
            macAddress,
            bleNvs,
            deviceConfig->bleAdvInterval.get());
    }
    LOGI("BLE disabled, using no-op driver");
    return std::make_shared<BleDriver>();
#else
    return std::make_shared<BleDriver>();
#endif
}
