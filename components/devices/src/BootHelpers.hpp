#pragma once

#include <BatteryManager.hpp>
#include <KernelStatus.hpp>
#include <NetworkConfig.hpp>
#include <NvsStore.hpp>
#include <Watchdog.hpp>
#include <devices/DeviceConfiguration.hpp>
#include <devices/DeviceDefinition.hpp>
#include <drivers/BleDriver.hpp>
#include <drivers/LedDriver.hpp>
#include <mqtt/MqttRoot.hpp>

#include <esp_transport.h>

#include <chrono>
#include <memory>
#include <string>

using namespace std::chrono;
using namespace cornucopia::ugly_duckling::devices;
using namespace cornucopia::ugly_duckling::kernel;

void performFactoryReset(const std::shared_ptr<LedDriver>& statusLed, bool completeReset);

std::shared_ptr<BatteryDriver> initBattery(const std::shared_ptr<DeviceDefinition>& deviceDefinition, const std::shared_ptr<I2CManager>& i2c);

void initNvsFlash();

std::shared_ptr<Watchdog> initWatchdog(seconds timeout);

/**
 * @param transport what to connect over instead of lwIP (the cellular modem), or nullptr
 */
std::shared_ptr<MqttRoot> initMqtt(const std::shared_ptr<ModuleStates>& states, const std::string& clientId, const std::shared_ptr<NetworkConfig>& networkConfig, StateSource& mqttReady, esp_transport_handle_t transport);

std::shared_ptr<BleDriver> initBle(
    const std::shared_ptr<DeviceConfiguration>& deviceConfig,
    const std::string& hostname,
    const std::string& deviceDescription,
    const std::string& firmwareVersion,
    const std::string& macAddress);
