# Wokwi Simulation

This directory contains Wokwi diagrams, custom chip simulations, and the local dev environment.

## Diagrams

Each diagram file corresponds to a hardware model. The active diagram is selected via `diagram.json` (a symlink — see `.gitignore`). Multiple diagrams share the same `wokwi.toml` build config.

| File                             | Hardware model               |
| -------------------------------- | ---------------------------- |
| `diagram.flow-control-mk8.json`  | Flow Control MK8 (ESP32-S3)  |
| `diagram.flow-control-mk10.json` | Flow Control MK10 (ESP32-C6) |
| `diagram.chicken-door.json`      | Chicken Door (ESP32-S3)      |
| `diagram.plot-control-mk6.json`  | Plot Control MK6 (ESP32-S3)  |

## Custom Chips

Custom chip simulations live in `chips/`. Each chip is a self-contained C file compiled to WebAssembly.

### File layout

```text
chips/
  <name>.chip.c       # Simulation logic (committed)
  <name>.chip.json    # Pin declaration (committed)
  <name>.chip.wasm    # Compiled binary (generated, gitignored)
  wokwi-api.h         # Wokwi API header (auto-downloaded, gitignored)
```

### Building

```sh
wokwi-cli chip compile chips/<name>.chip.c -o chips/<name>.chip.wasm
```

`wokwi-api.h` is downloaded automatically on first compile. Re-run this command after any edit to `.chip.c` before running the simulation.

### Registering a chip in `wokwi.toml`

```toml
[[chip]]
name = 'my-chip'
binary = 'chips/my-chip.chip.wasm'
```

### Wiring a chip in a diagram

The part type is `chip-<name>` where `<name>` matches the `name` field in `wokwi.toml`:

```json
{ "type": "chip-my-chip", "id": "u1", "top": 0, "left": 0, "attrs": {} }
```

Pin names in connections must exactly match the names declared in `<name>.chip.json`.

---

## Writing an I2C chip

### Pin declaration (`chip.json`)

```json
{
  "name": "MyChip",
  "author": "...",
  "pins": ["VCC", "GND", "SCL", "SDA"]
}
```

### Skeleton (`chip.c`)

```c
#include "wokwi-api.h"
#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t  reg;         // register byte from master
    uint8_t  byte_idx;    // byte position within current write transaction
    uint8_t  write_count; // number of data bytes received (excluding reg byte)
    uint8_t  write_data[8];
    uint8_t  read_idx;    // byte position within current read transaction
    // ... chip-specific state ...
} chip_state_t;

static chip_state_t chip;

static bool on_connect(void *user_data, uint32_t address, bool read) {
    if (read) {
        chip.read_idx = 0;      // master is reading — reset read cursor
    } else {
        chip.byte_idx = 0;      // master is writing — reset write state
        chip.write_count = 0;
    }
    return true; // ACK
}

static bool on_write(void *user_data, uint8_t data) {
    if (chip.byte_idx == 0) {
        chip.reg = data;        // first byte is always the register address
    } else if (chip.write_count < sizeof(chip.write_data)) {
        chip.write_data[chip.write_count++] = data;
    }
    chip.byte_idx++;
    return true; // ACK
}

static uint8_t on_read(void *user_data) {
    uint8_t result = 0;
    // Return bytes for chip.reg based on chip.read_idx
    chip.read_idx++;
    return result;
}

static void on_disconnect(void *user_data) {
    // Write-only transactions: process command after STOP
    if (chip.write_count >= 1) {
        // interpret chip.reg + chip.write_data[0..write_count-1]
    }
}

void chip_init(void) {
    memset(&chip, 0, sizeof(chip));
    i2c_init(&(i2c_config_t){
        .user_data  = NULL,
        .address    = 0x55,          // 7-bit I2C address
        .scl        = pin_init("SCL", INPUT),
        .sda        = pin_init("SDA", INPUT),
        .connect    = on_connect,
        .read       = on_read,
        .write      = on_write,
        .disconnect = on_disconnect,
    });
}
```

### I2C callback contract

| Callback                    | `read` / direction | When called                                | What to do                                      |
| --------------------------- | ------------------ | ------------------------------------------ | ----------------------------------------------- |
| `connect(addr, read=false)` | write              | START + addr+W                             | reset `byte_idx`, `write_count`                 |
| `write(data)`               | —                  | master sends byte                          | byte 0 → `reg`; subsequent bytes → `write_data` |
| `connect(addr, read=true)`  | read               | repeated-START + addr+R, or START + addr+R | reset `read_idx`                                |
| `read()`                    | —                  | master clocks a byte                       | return byte at `read_idx`, increment `read_idx` |
| `disconnect()`              | —                  | STOP                                       | process any completed write command             |

**Critical gotcha:** the `read` parameter in `connect` is `true` when the master is **reading** and `false` when **writing** — the opposite of what the I2C R/W̄ bit encodes. Reset read state on `read=true` and write state on `read=false`.

### Typical I2C transaction sequences

**Register write** (e.g. send a command):

```text
connect(read=false) → write(reg) → write(data0) → [write(data1)…] → disconnect()
```

**Register read** (write register address, then read data — repeated start):

```text
connect(read=false) → write(reg) → connect(read=true) → read() × N → disconnect()
```

**Register read** (two separate transactions — some ESP-IDF drivers do this):

```text
connect(read=false) → write(reg) → disconnect()
connect(read=true)  → read() × N → disconnect()
```

Both forms work correctly with the skeleton above because `chip.reg` is preserved across transactions (only `read_idx` and `byte_idx`/`write_count` are reset per transaction).

### Endianness

- `i2c_bus_read_bytes(handle, reg, 2, &uint16_val)` — reads into a `uint16_t*` on a little-endian MCU; first byte received becomes the **low** byte. Return `(uint8_t)value` at `read_idx==0` and `(uint8_t)(value>>8)` at `read_idx==1`.
- `bq27220_get_parameter_u16` — reads into a `uint8_t[2]` buffer and reconstructs big-endian: `(buf[0]<<8)|buf[1]`. Return `(uint8_t)(value>>8)` at `read_idx==0`.

---

## I2C library interoperability

This firmware uses two I2C libraries on the same physical bus:

- **`esp-idf-lib__i2cdev`** — used by `I2CManager` / `I2CDevice` and most peripheral drivers (INA219, etc.)
- **`espressif__i2c_bus`** — used by `espressif__bq27220` (BQ27220 fuel gauge driver)

Both call `i2c_new_master_bus()` internally. Only one can own the bus. The fix is in `Bq27220Driver`: call `device->probeRead()` **before** `i2c_bus_create()` so that `i2cdev` initialises and acquires the bus first. `i2c_bus_create()` then finds the handle via `i2c_master_get_bus_handle()` and reuses it without conflict.

Any future driver that uses `espressif__i2c_bus` on a port also used by `i2cdev` must follow the same pattern.
