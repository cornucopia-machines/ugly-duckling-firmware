#pragma once

#include <I2CManager.hpp>
#include <Task.hpp>
#include <drivers/BatteryDriver.hpp>

#include <bq27220.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

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
    .CCT = true,
    .CSYNC = false,
    .EDV_CMP = false,
    .SC = true,
    .FIXED_EDV0 = false,
    .FCC_LIM = true,
    .FC_FOR_VDQ = true,
    .IGNORE_SD = true,
    .SME0 = false,
};

// Operation Config A lives in the Configuration/Registers subclass at data memory
// address 0x9206 (tech ref §4.1.2). Bit 15 [TEMPS] selects the external thermistor
// on the TS pin as the source for Temperature(); with it clear -- the factory
// default of 0x0484 -- the gauge reports its on-chip sensor instead.
static constexpr uint16_t OPERATION_CONFIG_A_ADDRESS = 0x9206;
static constexpr uint16_t OPERATION_CONFIG_A_TEMPS = 0x8000;

// Control() (command 0x00) subcommands from tech ref Table 2-2. The espressif__bq27220
// component keeps its own bq27220_control() static, so we issue these over our own
// I2C device instead.
static constexpr uint8_t COMMAND_CONTROL = 0x00;
static constexpr uint8_t COMMAND_INTERNAL_TEMPERATURE = 0x28;
static constexpr uint16_t CONTROL_FULL_ACCESS_KEY = 0xFFFF;
static constexpr uint16_t CONTROL_ENTER_CFG_UPDATE = 0x0090;
static constexpr uint16_t CONTROL_EXIT_CFG_UPDATE_REINIT = 0x0091;

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
        LOGTI(BATTERY, "Initializing BQ27220 driver on SDA %s, SCL %s, address 0x%02X",
            sda->getName().c_str(), scl->getName().c_str(), address);

        // Probe the device so i2cdev initializes the I2C master bus first.
        // i2c_bus_create() (used by espressif__bq27220) calls i2c_master_get_bus_handle()
        // before trying i2c_new_master_bus(); if i2cdev already owns the bus it reuses the
        // handle instead of failing with ESP_ERR_INVALID_STATE. Result is ignored because
        // the BQ27220 may not ACK until fully powered.
        ESP_ERROR_THROW(device->probeRead());

        // Get the bus handle
        auto port = device->getBus()->port;
        i2c_config_t conf = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = sda->getGpio(),
            .scl_io_num = scl->getGpio(),
            .sda_pullup_en = false,
            .scl_pullup_en = false,
            .master = { .clk_speed = 100000 },
            .clk_flags = 0,
        };
        auto* bus = i2c_bus_create(port, &conf);

        // Initialize BQ27220 on existing bus
        bq27220_config_t bq27220_cfg = {
            .i2c_bus = bus,
            .cfg = &default_config,
            .cedv = &default_cedv,
        };
        gauge = bq27220_create(&bq27220_cfg);

        LOGTI(BATTERY, "Battery voltage at boot: %d mV / %.2f%%; temp = %.2f°C", getVoltage(), getPercentage(), getTemperature());
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
        return (bq27220_get_temperature(gauge) / 10.0) - 273.15;
    }

    /**
     * @brief Switches Temperature() over to the external thermistor on the TS pin.
     *
     * Only call this on boards that actually populate an NTC on TS: the gauge's
     * default linearization coefficients (Ext a Coef / Ext b Coef) are computed for a
     * Semitec 103AT-type part (10 kOhm +-1% at 25 C, B25/85 = 3435), and the polynomial
     * extrapolates without bound outside its fitted range. An open TS pin therefore
     * yields absurd readings rather than an error -- unlike the internal sensor, which
     * is clamped by Int Max Temp (613.1 K).
     *
     * [TEMPS] is a non-volatile data memory parameter, so it has to be written through
     * the CFGUPDATE sequence of tech ref section 6 rather than as a plain register write.
     */
    void useExternalThermistor() {
        auto config = bq27220_get_parameter_u16(gauge, OPERATION_CONFIG_A_ADDRESS);
        if ((config & OPERATION_CONFIG_A_TEMPS) != 0) {
            LOGTD(BATTERY, "BQ27220 already reads the external thermistor (Operation Config A = 0x%04X)", config);
            return;
        }

        LOGTD(BATTERY, "Switching BQ27220 to the external thermistor (Operation Config A = 0x%04X)", config);
        ESP_ERROR_THROW(bq27220_unseal(gauge));
        // Unsealing alone does not grant data memory access; FULL ACCESS does.
        controlCommand(CONTROL_FULL_ACCESS_KEY);
        controlCommand(CONTROL_FULL_ACCESS_KEY);

        controlCommand(CONTROL_ENTER_CFG_UPDATE);
        if (!waitForConfigUpdate(true)) {
            LOGTE(BATTERY, "BQ27220 did not enter CFGUPDATE mode, leaving the temperature source alone");
            return;
        }

        bq27220_set_parameter_u16(gauge, OPERATION_CONFIG_A_ADDRESS, config | OPERATION_CONFIG_A_TEMPS);

        controlCommand(CONTROL_EXIT_CFG_UPDATE_REINIT);
        if (!waitForConfigUpdate(false)) {
            LOGTE(BATTERY, "BQ27220 did not leave CFGUPDATE mode");
            return;
        }

        config = bq27220_get_parameter_u16(gauge, OPERATION_CONFIG_A_ADDRESS);
        if ((config & OPERATION_CONFIG_A_TEMPS) == 0) {
            LOGTE(BATTERY, "BQ27220 rejected the external thermistor setting (Operation Config A = 0x%04X)", config);
        } else {
            LOGTI(BATTERY, "BQ27220 now reads the external thermistor (Operation Config A = 0x%04X)", config);
        }
        bq27220_seal(gauge);
    }

    /**
     * @brief Reads BatteryStatus(), the gauge's flag word.
     *
     * Read the word once and pick the flags out of it rather than making one call per flag:
     * every call is a separate I2C transaction, and flags that disagree about which moment
     * they describe are worse than useless when diagnosing the gauge.
     *
     * @return the flags, or std::nullopt when the read failed.
     */
    std::optional<battery_status_t> getBatteryStatus() {
        battery_status_t status {};
        if (bq27220_get_battery_status(gauge, &status) != ESP_OK) {
            return std::nullopt;
        }
        return status;
    }

    /**
     * @brief Returns the on-chip sensor temperature in Celsius.
     *
     * InternalTemperature() always reports the internal sensor regardless of [TEMPS],
     * which makes it a useful cross-check against getTemperature(). The
     * espressif__bq27220 component has no accessor for it, so read the command
     * directly; like every gauge word it comes back LSB first, in units of 0.1 K.
     */
    double getInternalTemperature() {
        return (device->readRegWord(COMMAND_INTERNAL_TEMPERATURE) / 10.0) - 273.15;
    }

    std::optional<seconds> getTimeToEmpty() override {
        return minutes { bq27220_get_time_to_empty(gauge) };
    }

    std::optional<seconds> getTimeToFull() {
        return minutes { bq27220_get_time_to_full(gauge) };
    }

private:
    void controlCommand(uint16_t subcommand) {
        // Control() expects the subcommand LSB first, which is how writeRegWord()
        // lays out a uint16_t on our little-endian targets.
        device->writeRegWord(COMMAND_CONTROL, subcommand);
        Task::delay(10ms);
    }

    bool waitForConfigUpdate(bool expected) {
        // The gauge can take up to a second to enter or leave CFGUPDATE mode.
        for (int attempt = 0; attempt < 40; attempt++) {
            operation_status_t status {};
            if (bq27220_get_operation_status(gauge, &status) == ESP_OK && status.CFGUPDATE == expected) {
                return true;
            }
            Task::delay(50ms);
        }
        return false;
    }

    std::shared_ptr<I2CDevice> device;
    bq27220_handle_t gauge = nullptr;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
