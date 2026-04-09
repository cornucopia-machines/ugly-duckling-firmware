#pragma once

#include <Pin.hpp>
#include <drivers/LedDriver.hpp>

#include <peripherals/Peripheral.hpp>

#include <devices/DeviceDefinition.hpp>

using namespace cornucopia::ugly_duckling::kernel;

namespace cornucopia::ugly_duckling::devices {

class GenericDevice : public DeviceDefinition {
public:
    GenericDevice()
        : DeviceDefinition({
              .model = "generic",
              .revision = 1,
#if defined(CONFIG_IDF_TARGET_ESP32S3)
              .boot = GPIO_NUM_0,
              .status = GPIO_NUM_48,
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
              .boot = GPIO_NUM_9,
              .status = GPIO_NUM_8,
#else
#error "Unsupported target"
#endif
          }) {
    }

protected:
    void registerDeviceSpecificPeripheralFactories(const std::shared_ptr<PeripheralManager>& _peripheralManager, const PeripheralServices& _services, const std::shared_ptr<DeviceSettings>& _settings) override {
    }
};

}    // namespace cornucopia::ugly_duckling::devices
