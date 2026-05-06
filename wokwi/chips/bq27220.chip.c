// Minimal BQ27220 fuel gauge simulation for Wokwi.
//
// Implements just enough of the I2C protocol to let bq27220_create() succeed
// and bq27220_get_voltage() return a sensible value. The key insight is that
// the driver's create() function checks four data-memory values (design_cap,
// EMF, T0, DOD20) against the defaults in default_cedv. If they match, it
// skips the full profile update and returns immediately. We always report those
// matching values so the skip path is taken every time.
//
// I2C protocol handled:
//   - Write to COMMAND_CONTROL (0x00): stores the 2-byte subcommand
//   - Read from COMMAND_MAC_DATA (0x40): returns LE response to last control cmd
//   - Write to COMMAND_SELECT_SUBCLASS (0x3E): stores 2-byte DM address
//   - Read from COMMAND_MAC_DATA (0x40): returns BE response for last DM address
//   - Read from COMMAND_OPERATION_STATUS (0x3A): SEC=UNSEALED, INITCOMP=1
//   - Read from COMMAND_DESIGN_CAPACITY (0x3C): 2000 mAh
//   - Read from COMMAND_VOLTAGE (0x08): 3800 mV

#include "wokwi-api.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Standard command registers
#define REG_CONTROL          0x00
#define REG_VOLTAGE          0x08
#define REG_OPERATION_STATUS 0x3A
#define REG_DESIGN_CAPACITY  0x3C
#define REG_SELECT_SUBCLASS  0x3E
#define REG_MAC_DATA         0x40

// Control() MAC subcommands
#define CTRL_DEVICE_NUMBER 0x0001
#define CTRL_FW_VERSION    0x0002
#define CTRL_HW_VERSION    0x0003

// Data memory addresses (from bq27220_data_memory.h dm_table)
#define DM_EMF   0x92A7
#define DM_T0    0x92AD
#define DM_DOD20 0x92C1

// Values matching default_cedv in Bq27220Driver.hpp so the skip path is taken
#define SIM_VOLTAGE_MV  3800
#define SIM_DESIGN_CAP  2000
#define SIM_EMF         3670
#define SIM_T0          4547
#define SIM_DOD20       3969

typedef enum { MAC_NONE, MAC_CONTROL, MAC_SUBCLASS } mac_mode_t;

typedef struct {
    uint8_t    reg;
    uint8_t    byte_idx;
    uint8_t    write_count;
    uint8_t    write_data[8];
    uint8_t    read_idx;
    uint16_t   last_control;
    uint16_t   last_subclass;
    mac_mode_t mac_mode;
} chip_state_t;

static chip_state_t chip;

static bool on_connect(void *user_data, uint32_t address, bool read) {
    if (read) {
        // Master is reading from chip (repeated-start or read-only transaction)
        chip.read_idx = 0;
    } else {
        // Master is writing to chip; reset write state for this transaction
        chip.byte_idx = 0;
        chip.write_count = 0;
    }
    return true;  // ACK
}

static bool on_write(void *user_data, uint8_t data) {
    if (chip.byte_idx == 0) {
        chip.reg = data;
    } else if (chip.write_count < (uint8_t)sizeof(chip.write_data)) {
        chip.write_data[chip.write_count++] = data;
    }
    chip.byte_idx++;
    return true;  // ACK
}

static uint8_t on_read(void *user_data) {
    uint8_t result = 0;

    if (chip.reg == REG_MAC_DATA) {
        uint16_t value = 0;
        bool big_endian = false;

        if (chip.mac_mode == MAC_SUBCLASS) {
            // bq27220_get_parameter_u16 reads big-endian from MAC_DATA
            big_endian = true;
            switch (chip.last_subclass) {
                case DM_EMF:   value = SIM_EMF;   break;
                case DM_T0:    value = SIM_T0;    break;
                case DM_DOD20: value = SIM_DOD20; break;
                default:       value = 0;         break;
            }
        } else if (chip.mac_mode == MAC_CONTROL) {
            // bq27220_read_u16 reads into uint16_t* so it's little-endian
            switch (chip.last_control) {
                case CTRL_DEVICE_NUMBER: value = 0x0220; break;
                case CTRL_FW_VERSION:    value = 0x0100; break;
                case CTRL_HW_VERSION:    value = 0x0100; break;
                default:                 value = 0;      break;
            }
        }

        if (big_endian) {
            result = (chip.read_idx == 0) ? (uint8_t)(value >> 8) : (uint8_t)value;
        } else {
            result = (chip.read_idx == 0) ? (uint8_t)value : (uint8_t)(value >> 8);
        }

    } else if (chip.reg == REG_VOLTAGE) {
        uint16_t v = SIM_VOLTAGE_MV;
        result = (chip.read_idx == 0) ? (uint8_t)v : (uint8_t)(v >> 8);

    } else if (chip.reg == REG_OPERATION_STATUS) {
        // Low byte: CALMD(0)=0, SEC(1-2)=0b10→bit2=1→0x04, INITCOMP(5)=1→0x20
        // SEC=0b10 is OPERATION_STATUS_SEC_UNSEALED; INITCOMP makes gauge ready
        result = (chip.read_idx == 0) ? 0x24 : 0x00;

    } else if (chip.reg == REG_DESIGN_CAPACITY) {
        uint16_t cap = SIM_DESIGN_CAP;
        result = (chip.read_idx == 0) ? (uint8_t)cap : (uint8_t)(cap >> 8);
    }

    chip.read_idx++;
    return result;
}

static void on_disconnect(void *user_data) {
    // Process completed write-only transactions (write_count >= 2 means data
    // bytes were present, not just the register address byte).
    if (chip.write_count >= 2) {
        uint16_t value = chip.write_data[0] | ((uint16_t)chip.write_data[1] << 8);
        if (chip.reg == REG_CONTROL) {
            chip.last_control = value;
            chip.mac_mode = MAC_CONTROL;
        } else if (chip.reg == REG_SELECT_SUBCLASS) {
            chip.last_subclass = value;
            chip.mac_mode = MAC_SUBCLASS;
        }
    }
}

void chip_init(void) {
    memset(&chip, 0, sizeof(chip));
    i2c_init(&(i2c_config_t){
        .user_data  = NULL,
        .address    = 0x55,
        .scl        = pin_init("SCL", INPUT),
        .sda        = pin_init("SDA", INPUT),
        .connect    = on_connect,
        .read       = on_read,
        .write      = on_write,
        .disconnect = on_disconnect,
    });
}
