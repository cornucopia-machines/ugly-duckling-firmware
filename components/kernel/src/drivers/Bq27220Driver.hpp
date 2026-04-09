#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include <bq27220.h>

#include <I2CManager.hpp>
#include <drivers/BatteryDriver.hpp>

using namespace cornucopia::ugly_duckling::kernel;

namespace cornucopia::ugly_duckling::kernel::drivers {

// Default Gauging Parameter
static const parameter_cedv_t default_cedv = {
    .full_charge_cap = 2000,
    .design_cap = 2000,
    .reserve_cap = 0,
    .near_full = 1800,
    .self_discharge_rate = 20,
    .EDV0 = 3490,
    .EDV1 = 3511,
    .EDV2 = 3535,
    .EMF = 3670,
    .C0 = 115,
    .R0 = 968,
    .T0 = 4547,
    .R1 = 4764,
    .TC = 11,
    .C1 = 0,
    .DOD0 = 4147,
    .DOD10 = 4002,
    .DOD20 = 3969,
    .DOD30 = 3938,
    .DOD40 = 3880,
    .DOD50 = 3824,
    .DOD60 = 3794,
    .DOD70 = 3753,
    .DOD80 = 3677,
    .DOD90 = 3574,
    .DOD100 = 3490,
};

// Default Gauging Config
static const gauging_config_t default_config = {
    .CCT = 1,
    .CSYNC = 0,
    .EDV_CMP = 0,
    .SC = 1,
    .FIXED_EDV0 = 0,
    .FCC_LIM = 1,
    .FC_FOR_VDQ = 1,
    .IGNORE_SD = 1,
    .SME0 = 0,
};

class Bq27220Driver final : public BatteryDriver {
public:
    Bq27220Driver(
        const std::shared_ptr<I2CManager>& i2c,
        const InternalPinPtr& sda,
        const InternalPinPtr& scl,
        const BatteryParameters& parameters)
        : Bq27220Driver(i2c, sda, scl, 0x55, parameters) {
    }

    Bq27220Driver(
        const std::shared_ptr<I2CManager>& i2c,
        const InternalPinPtr& sda,
        const InternalPinPtr& scl,
        uint8_t address,
        const BatteryParameters& parameters)
        : BatteryDriver(parameters)
        , device(i2c->createDevice("battery:bq27220", sda, scl, address)) {
        LOGI("Initializing BQ27220 driver on SDA %s, SCL %s, address 0x%02X",
            sda->getName().c_str(), scl->getName().c_str(), address);

        // Check if we can communicate with the device and initialize bus
        ESP_ERROR_THROW(device->probeRead());

        // Get the bus handle
        auto port = device->getBus()->port;
        i2c_config_t conf = {
            .mode = I2C_MODE_MASTER,
            .master = { .clk_speed = 100000 },
        };
        auto* bus = i2c_bus_create(port, &conf);

        // Initialize BQ27220 on existing bus
        bq27220_config_t bq27220_cfg = {
            .i2c_bus = bus,
            .cfg = &default_config,
            .cedv = &default_cedv,
        };
        gauge = bq27220_create(&bq27220_cfg);
    }

    int getVoltage() override {
        return bq27220_get_voltage(gauge);
    }

    double getPercentage() override {
        return bq27220_get_state_of_charge(gauge);
    }

    std::optional<double> getCurrent() override {
        return bq27220_get_current(gauge);
    }

    double getTemperature() {
        return bq27220_get_temperature(gauge) / 10.0 - 273.15;
    }

    std::optional<seconds> getTimeToEmpty() override {
        return minutes { bq27220_get_time_to_empty(gauge) };
    }

    std::optional<seconds> getTimeToFull() {
        return minutes { bq27220_get_time_to_full(gauge) };
    }

private:
    std::shared_ptr<I2CDevice> device;
    bq27220_handle_t gauge = nullptr;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
