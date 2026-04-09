#pragma once

#include <ina219.h>

#include <I2CManager.hpp>

using namespace cornucopia::ugly_duckling::kernel;

namespace cornucopia::ugly_duckling::kernel::drivers {

struct Ina219Parameters {
    ina219_bus_voltage_range_t uRange;
    ina219_gain_t gain;
    ina219_resolution_t uResolution;
    ina219_resolution_t iResolution;
    ina219_mode_t mode;
    uint16_t shuntMilliOhm;
};

class Ina219Driver final {
public:
    static constexpr uint8_t DEFAULT_ADDRESS = 0x40;

    Ina219Driver(
        const std::shared_ptr<I2CManager>& i2c,
        const I2CConfig& config,
        const Ina219Parameters params)
        : initParams(params) {
        LOGI("Initializing INA219 driver, %s", config.toString().c_str());

        auto bus = i2c->getBusFor(config.sda, config.scl);
        ESP_ERROR_THROW(ina219_init_desc(&device, config.address,
            bus->port, bus->sda->getGpio(), bus->scl->getGpio()));
        ESP_ERROR_THROW(ina219_init(&device));

        LOGD("Configuring INA219");
        ESP_ERROR_THROW(ina219_configure(&device,
            params.uRange, params.gain, params.uResolution, params.iResolution, params.mode));
        enabled = true;

        LOGD("Calibrating INA219");
        ESP_ERROR_THROW(ina219_calibrate(&device, (float) params.shuntMilliOhm / 1000.0F));

        LOGD("Finished calibrating, disabling INA219 until needed");
        setEnabled(false);
    }

    void setEnabled(bool enable) {
        if (enabled != enable) {
            enabled = enable;
            ESP_ERROR_THROW(ina219_configure(&device,
                initParams.uRange, initParams.gain, initParams.uResolution, initParams.iResolution, enabled ? initParams.mode : INA219_MODE_POWER_DOWN));
        }
    }

    double getBusVoltage() {
        if (!enabled) {
            LOGW("INA219 is disabled");
            return 0.0;
        }

        float voltage;
        ESP_ERROR_THROW(ina219_get_bus_voltage(&device, &voltage));
        return voltage;
    }

    double getShuntVoltage() {
        if (!enabled) {
            LOGW("INA219 is disabled");
            return 0.0;
        }

        float voltage;
        ESP_ERROR_THROW(ina219_get_shunt_voltage(&device, &voltage));
        return voltage;
    }

    double getCurrent() {
        if (!enabled) {
            LOGW("INA219 is disabled");
            return 0.0;
        }

        float current;
        ESP_ERROR_THROW(ina219_get_current(&device, &current));
        return current;
    }

    double getPower() {
        if (!enabled) {
            LOGW("INA219 is disabled");
            return 0.0;
        }

        float power;
        ESP_ERROR_THROW(ina219_get_power(&device, &power));
        return power;
    }

private:
    ina219_t device {};
    const Ina219Parameters initParams;
    bool enabled;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
