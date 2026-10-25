#pragma once
#include <EspError.hpp>
#include <I2CManager.hpp>
#include <Result.hpp>

#include <ina219.h>

#include <memory>

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

    static Result<std::shared_ptr<Ina219Driver>> create(
        const std::shared_ptr<I2CManager>& i2c,
        const I2CConfig& config,
        const Ina219Parameters params) {
        LOGI("Initializing INA219 driver, %s", config.toString().c_str());

        auto bus = TRY(i2c->getBusFor(config.sda, config.scl));
        auto driver = std::shared_ptr<Ina219Driver>(new Ina219Driver(params));
        auto& device = driver->device;
        ESP_TRY(ina219_init_desc(&device, config.address,
            bus->port, bus->sda->getGpio(), bus->scl->getGpio()));
        ESP_TRY(ina219_init(&device));

        LOGD("Configuring INA219");
        ESP_TRY(ina219_configure(&device,
            params.uRange, params.gain, params.uResolution, params.iResolution, params.mode));
        driver->enabled = true;

        LOGD("Calibrating INA219");
        ESP_TRY(ina219_calibrate(&device, (float) params.shuntMilliOhm / 1000.0F));

        LOGD("Finished calibrating, disabling INA219 until needed");
        TRY(driver->setEnabled(false));
        return driver;
    }

    ~Ina219Driver() {
        ina219_free_desc(&device);
    }

    Ina219Driver(const Ina219Driver&) = delete;
    Ina219Driver& operator=(const Ina219Driver&) = delete;
    Ina219Driver(Ina219Driver&&) = delete;
    Ina219Driver& operator=(Ina219Driver&&) = delete;

private:
    Result<> setEnabled(bool enable) {
        if (enabled != enable) {
            enabled = enable;
            ESP_TRY(ina219_configure(&device,
                initParams.uRange, initParams.gain, initParams.uResolution, initParams.iResolution, enabled ? initParams.mode : INA219_MODE_POWER_DOWN));
        }
        return {};
    }

    ina219_t device {};
    const Ina219Parameters initParams;
    bool enabled = false;

    explicit Ina219Driver(const Ina219Parameters& params)
        : initParams(params) {
    }
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
