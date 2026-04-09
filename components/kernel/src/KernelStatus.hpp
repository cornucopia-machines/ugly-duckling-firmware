#pragma once

#include <chrono>
#include <concepts>
#include <functional>
#include <optional>

#include <esp_system.h>

#include <nvs_flash.h>

#include <freertos/FreeRTOS.h>    // NOLINT(misc-header-include-cycle)

#include <I2CManager.hpp>
#include <MacAddress.hpp>
#include <PowerManager.hpp>
#include <StateManager.hpp>
#include <drivers/LedDriver.hpp>
#include <drivers/RtcDriver.hpp>
#include <drivers/SwitchManager.hpp>
#include <drivers/WiFiDriver.hpp>
#include <mqtt/MqttDriver.hpp>

using namespace std::chrono;
using namespace std::chrono_literals;
using namespace cornucopia::ugly_duckling::kernel::drivers;
using namespace cornucopia::ugly_duckling::kernel::mqtt;

namespace cornucopia::ugly_duckling::kernel {

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
static RTC_DATA_ATTR int bootCount = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

class KernelStatusTask;

struct ModuleStates {
private:
    StateManager manager;
    friend class KernelStatusTask;

public:
    StateSource networkConnecting = manager.createStateSource("network-connecting");
    StateSource networkReady = manager.createStateSource("network-ready");
    StateSource configPortalRunning = manager.createStateSource("config-portal-running");
    StateSource rtcInSync = manager.createStateSource("rtc-in-sync");
    StateSource mqttReady = manager.createStateSource("mqtt-ready");
    StateSource kernelReady = manager.createStateSource("kernel-ready");
};

class KernelStatusTask {
public:
    static void init(const std::shared_ptr<LedDriver>& statusLed, const std::shared_ptr<ModuleStates>& states) {
        Task::run("status-update", 3072, [statusLed, states](Task&) {
            updateState(statusLed, states);
        });
    }

private:
    enum class KernelState : uint8_t {
        Booting,
        NetworkConnecting,
        NetworkConfiguring,
        RtcSyncing,
        MqttConnecting,
        InitFinishing,
        Transmitting,
        Idle
    };

    static void updateState(const std::shared_ptr<LedDriver>& statusLed, const std::shared_ptr<ModuleStates>& states) {
        KernelState state = KernelState::Booting;
        while (true) {
            KernelState newState;
            if (states->configPortalRunning.isSet()) {
                // We are waiting for the user to configure the network
                newState = KernelState::NetworkConfiguring;
            } else if (states->networkConnecting.isSet()) {
                // We are waiting for network connection
                newState = KernelState::NetworkConnecting;
            } else if (!states->rtcInSync.isSet()) {
                newState = KernelState::RtcSyncing;
            } else if (!states->mqttReady.isSet()) {
                // We are waiting for MQTT connection
                newState = KernelState::MqttConnecting;
            } else if (!states->kernelReady.isSet()) {
                // We are waiting for init to finish
                newState = KernelState::InitFinishing;
            } else if (states->networkReady.isSet()) {
                newState = KernelState::Transmitting;
            } else {
                newState = KernelState::Idle;
            }

            if (newState != state) {
                LOGD("Kernel state changed from %d to %d",
                    static_cast<int>(state), static_cast<int>(newState));
                state = newState;
                switch (newState) {
                    case KernelState::Booting:
                        statusLed->turnOff();
                        break;
                    case KernelState::NetworkConnecting:
                        statusLed->blink(200ms);
                        break;
                    case KernelState::NetworkConfiguring:
                        statusLed->blinkPattern({ 100ms, -100ms, 100ms, -100ms, 100ms, -500ms });
                        break;
                    case KernelState::RtcSyncing:
                        statusLed->blink(500ms);
                        break;
                    case KernelState::MqttConnecting:
                        statusLed->blink(1000ms);
                        break;
                    case KernelState::InitFinishing:
                        statusLed->blink(1500ms);
                        break;
                    case KernelState::Transmitting:
                    case KernelState::Idle:
                        statusLed->turnOff();
                        break;
                };
            }
            states->manager.awaitStateChange();
        }
    }
};

}    // namespace cornucopia::ugly_duckling::kernel
