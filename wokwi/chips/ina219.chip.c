// Minimal INA219 power monitor simulation for Wokwi.
//
// Implements just enough of the I2C protocol to let ina219_init() and
// ina219_calibrate() succeed, and measurement functions return sensible values.
// All registers are 16-bit, big-endian (MSB first), matching the wire format
// expected by esp-idf-lib__ina219 which byte-swaps after reading.
//
// I2C protocol handled:
//   - Read/write REG_CONFIG (0x00): tracks config; returns power-on default until written
//   - Read REG_SHUNT_U (0x01): fixed shunt voltage raw value
//   - Read REG_BUS_U (0x02): fixed bus voltage raw value (3800 mV)
//   - Read REG_POWER (0x03) / REG_CURRENT (0x04): zero
//   - Write REG_CALIBRATION (0x05): stores value (ACKed, ignored for reads)

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

// Bus voltage: 3800 mV
// ina219_get_bus_voltage: *voltage = (raw >> 3) * 0.004  →  raw = (3800 / 4) << 3 = 7600
#define SIM_BUS_RAW    7600

// Shunt voltage: 10 mV (small idle current through shunt)
// ina219_get_shunt_voltage: *voltage = raw / 100000.0  →  raw = 0.010 * 100000 = 1000
#define SIM_SHUNT_RAW  1000

typedef struct {
    uint8_t  reg;
    uint8_t  byte_idx;
    uint8_t  write_count;
    uint8_t  write_data[8];
    uint8_t  read_idx;
    uint16_t config;
    uint16_t calibration;
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

    switch (chip.reg) {
        case REG_CONFIG:      value = chip.config;      break;
        case REG_SHUNT_U:     value = SIM_SHUNT_RAW;   break;
        case REG_BUS_U:       value = SIM_BUS_RAW;     break;
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
