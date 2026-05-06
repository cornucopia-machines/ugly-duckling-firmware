// Minimal INA219 power monitor simulation for Wokwi.
//
// Implements just enough of the I2C protocol to let ina219_init() and
// ina219_calibrate() succeed, and measurement functions return sensible values.
// All registers are 16-bit, big-endian (MSB first), matching the wire format
// expected by esp-idf-lib__ina219 which byte-swaps after reading.
//
// I2C protocol handled:
//   - Read/write REG_CONFIG (0x00): tracks config; returns power-on default until written
//   - Read REG_SHUNT_U (0x01): shunt voltage derived from current attr and SIM_R_SHUNT_MOHM
//   - Read REG_BUS_U (0x02): bus voltage from voltage attribute (mV, default 3800)
//   - Read REG_CURRENT (0x04): current via INA219 formula (shunt_raw * cal / 4096)
//   - Read REG_POWER (0x03): zero
//   - Write REG_CALIBRATION (0x05): stored and used for current register computation
//
// Configurable via Wokwi diagram attrs:
//   "voltage": bus voltage in mV (e.g. "3800")
//   "current": load current in mA (e.g. "100"), default 0

#include "wokwi-api.h"
#include <stdint.h>
#include <string.h>

#define REG_CONFIG      0x00
#define REG_SHUNT_U     0x01
#define REG_BUS_U       0x02
#define REG_POWER       0x03
#define REG_CURRENT     0x04
#define REG_CALIBRATION 0x05

// INA219 power-on default configuration (from datasheet / driver DEF_CONFIG)
#define DEFAULT_CONFIG 0x399F

// Default bus voltage in mV; raw register = (mV / 4) << 3
#define SIM_VOLTAGE_MV    3800

// Assumed shunt resistance in mΩ — matches the commented-out MK10 config.
// raw_shunt = current_mA * r_shunt_mΩ / 10  (each shunt LSB = 10 µV)
// raw_current = raw_shunt * calibration / 4096  (INA219 datasheet formula)
#define SIM_R_SHUNT_MOHM  50

typedef struct {
    uint8_t  reg;
    uint8_t  byte_idx;
    uint8_t  write_count;
    uint8_t  write_data[8];
    uint8_t  read_idx;
    uint16_t config;
    uint16_t calibration;
    uint32_t voltage_attr;
    uint32_t current_attr;
} chip_state_t;

static chip_state_t chip;

static bool on_connect(void *user_data, uint32_t address, bool read) {
    if (read) {
        chip.read_idx = 0;
    } else {
        chip.byte_idx = 0;
        chip.write_count = 0;
    }
    return true;
}

static bool on_write(void *user_data, uint8_t data) {
    if (chip.byte_idx == 0) {
        chip.reg = data;
    } else if (chip.write_count < (uint8_t)sizeof(chip.write_data)) {
        chip.write_data[chip.write_count++] = data;
    }
    chip.byte_idx++;
    return true;
}

static uint8_t on_read(void *user_data) {
    uint16_t value = 0;

    int32_t raw_shunt = (int32_t)attr_read(chip.current_attr) * SIM_R_SHUNT_MOHM / 10;

    switch (chip.reg) {
        case REG_CONFIG:      value = chip.config;      break;
        case REG_SHUNT_U:     value = (uint16_t)(int16_t)raw_shunt; break;
        case REG_BUS_U:       value = (uint16_t)((attr_read(chip.voltage_attr) / 4) << 3); break;
        case REG_CURRENT:     value = (uint16_t)(int16_t)(raw_shunt * (int32_t)chip.calibration / 4096); break;
        case REG_CALIBRATION: value = chip.calibration; break;
        default:              value = 0;                break;
    }

    // Big-endian: MSB first
    uint8_t result = (chip.read_idx == 0) ? (uint8_t)(value >> 8) : (uint8_t)value;
    chip.read_idx++;
    return result;
}

static void on_disconnect(void *user_data) {
    if (chip.write_count >= 2) {
        // Driver sends big-endian (byte-swaps before writing via i2c_dev_write_reg)
        uint16_t value = ((uint16_t)chip.write_data[0] << 8) | chip.write_data[1];
        switch (chip.reg) {
            case REG_CONFIG:      chip.config      = value; break;
            case REG_CALIBRATION: chip.calibration = value; break;
            default: break;
        }
    }
}

void chip_init(void) {
    memset(&chip, 0, sizeof(chip));
    chip.config = DEFAULT_CONFIG;
    chip.voltage_attr = attr_init("voltage", SIM_VOLTAGE_MV);
    chip.current_attr = attr_init("current", 0);
    i2c_init(&(i2c_config_t){
        .user_data  = NULL,
        .address    = 0x40,
        .scl        = pin_init("SCL", INPUT),
        .sda        = pin_init("SDA", INPUT),
        .connect    = on_connect,
        .read       = on_read,
        .write      = on_write,
        .disconnect = on_disconnect,
    });
}
