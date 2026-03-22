#pragma once

#include <Pin.hpp>
#include <drivers/LedDriver.hpp>

#include <peripherals/Peripheral.hpp>

#include <devices/DeviceDefinition.hpp>

using namespace farmhub::kernel;

namespace farmhub::devices {

class MkXSettings
    : public DeviceSettings {
public:
    MkXSettings()
        : DeviceSettings("mkx") {
    }
};

class UglyDucklingMkX : public DeviceDefinition<MkXSettings> {
public:
    UglyDucklingMkX()
        : DeviceDefinition({ .model = "mkx", .revision = 1, .boot = GPIO_NUM_9, .status = GPIO_NUM_1 }) {
    }

protected:
    void registerDeviceSpecificPeripheralFactories(const std::shared_ptr<PeripheralManager>& /*peripheralManager*/, const PeripheralServices& /*services*/, const std::shared_ptr<MkXSettings>& /*settings*/) override {
    }
};

}    // namespace farmhub::devices
