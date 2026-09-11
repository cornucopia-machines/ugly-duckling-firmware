#pragma once

#include <FirmwareRollback.hpp>
#include <HardwareVersion.hpp>
#include <NetworkConfig.hpp>
#include <PowerManager.hpp>
#include <config/ConfigState.hpp>
#include <devices/DeviceDefinition.hpp>
#include <mqtt/MqttRoot.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

using namespace std::chrono;
using namespace cornucopia::ugly_duckling::devices;
using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::config;
using namespace cornucopia::ugly_duckling::kernel::mqtt;

enum class InitState : std::uint8_t {
    Success = 0,
    PeripheralError = 1,
    FunctionError = 2,
};

/**
 * @brief Publishes `boot` (NoRetain, QoS 1): diagnostics and per-peripheral/function error
 * feedback (docs/Configuration.md, "BOOT, SYNC, UPDATE").
 */
void publishBootMessage(
    const std::shared_ptr<MqttRoot>& mqttRoot,
    esp_reset_reason_t resetReason,
    const std::string& firmwareVersion,
    const std::string& macAddress,
    const std::shared_ptr<NetworkConfig>& networkConfig,
    InitState initState,
    const JsonArray& peripheralsInitJson,
    const JsonArray& functionsInitJson,
    const std::shared_ptr<PowerManager>& powerManager,
    const std::shared_ptr<DeviceDefinition>& deviceDefinition,
    const std::optional<HardwareVersion>& hardwareVersion,
    const std::optional<RejectionCode>& rejectionToReport,
    const std::optional<RejectionCode>& firmwareDownloadRejection,
    const std::optional<RollbackDetection>& rollback);
