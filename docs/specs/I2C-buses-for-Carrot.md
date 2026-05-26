# External I2C for MK10 on ESP32-C6 (Carrot)

## Problem

The MK10 hardware needs two independent I2C buses:

| Bus      | SDA           | SCL           | Devices                                                                  |
| -------- | ------------- | ------------- | ------------------------------------------------------------------------ |
| Internal | `GPIO_NUM_3`  | `GPIO_NUM_2`  | `BQ27220` battery fuel gauge, `INA219` current monitor                   |
| External | `GPIO_NUM_11` | `GPIO_NUM_10` | Pluggable I2C peripherals — primarily the **Spadefoot Toad** soil sensor |

ESP32-C6 has only one fully matrix-routable I2C peripheral. From
`components/soc/esp32c6/include/soc/soc_caps.h`:

```c
#define SOC_HP_I2C_NUM  (1U)   // 1× general-purpose I2C, GPIO matrix routable
#define SOC_LP_I2C_NUM  (1U)   // 1× low-power I2C — pin-locked to GPIO6/7
```

`I2C_NUM_0` (HP_I2C0) can be routed anywhere; `LP_I2C_NUM_0` is physically tied
to GPIO6/7 — which MK10 does not bring out. Neither pin pair MK10 uses
(GPIO2/3, GPIO10/11) matches that lock, so we cannot give both buses to
hardware peripherals on the existing board.

This was a non-issue on Spinach (ESP32-S3): `SOC_HP_I2C_NUM = 2`, both ports
matrix-routable, so MK9 used hardware I2C on both buses.

## Decision

Two boards, two strategies, both small.

- **MK10** is salvaged by adding a **Spadefoot-Toad-specific bitbang
  driver**. The internal bus stays on HP_I2C0 exactly as today. The external
  bus accepts only the Spadefoot Toad on MK10. We accept that constraint
  rather than building a general-purpose software I²C transport for a single
  board generation.
- **MK11+** (next-generation hardware) will re-route the **internal** bus to
  GPIO6/7 so it lands on LP_I2C0, freeing HP_I2C0 for the external bus. From
  HP code, LP_I2C0 is driven by the regular `i2c_master*\*` API — only the
  port number and the clock-source field change. No bitbang anywhere. Every
  existing I²C peripheral driver in the tree (Spadefoot Toad, SHT3x, BH1750,
  TSL2591, SHT2x/Si7021, INA219, BQ27220) can be plugged in to either bus.

## MK10 plan: `SpadefootToadSensorWithBitbangI2C`

A single new sensor class, in the same header as `SpadefootToadSensor`, with
the bitbang transport inlined as private members. **No changes to
`I2CManager` or `I2CDevice`.** The standard hardware-I²C `SpadefootToadSensor`
keeps working on Spinach / MK11+ / Wokwi.

### File layout

Everything lives in
`components/peripherals/src/peripherals/environment/SpadefootToadSensor.hpp`:

- A new private base class `SpadefootToadSensorBase` holds the shared
  protocol surface: command byte constants (`CMD_TRIGGER`, `CMD_READ`,
  `CMD_READ_RAW`, `CMD_READ_CALIBRATION`, the `CMD_GET_*` family, etc.),
  manufacturer/device-ID constants, the XOR-checksum helper, the
  `SpadefootToadReading` struct, the calibration-EEPROM parsing, and the
  `DebouncedMeasurement` body. It expects subclasses to implement the three
  transport primitives the protocol needs:

  | Operation                             | Used for                                                                            |
  | ------------------------------------- | ----------------------------------------------------------------------------------- |
  | `writeByte(cmd)`                      | `CMD_TRIGGER`, `CMD_CALIBRATE_*`, `CMD_FACTORY_RESET`                               |
  | `readWord(cmd) → uint16_t`            | `CMD_GET_MFR_ID`, `CMD_GET_DEVICE_ID`, `CMD_GET_FIRMWARE_REV`, `CMD_GET_DEVICE_REV` |
  | `readBytes(cmd, n) → vector<uint8_t>` | `CMD_READ` (10 B), `CMD_READ_RAW` (14 B), `CMD_READ_CALIBRATION` (17 B)             |

- The existing `SpadefootToadSensor` becomes a thin subclass that forwards
  the three primitives to an `I2CDevice` from `I2CManager`. No protocol
  logic moves; everything that already worked keeps working.
- A new `SpadefootToadSensorWithBitbangI2C` subclass implements the same
  three primitives against an inlined bitbang transport (raw `InternalPinPtr`
  for SDA/SCL, no `I2CManager` involvement). The name is deliberately
  verbose — it should be obvious at the call site why this class exists.
- A leading file-level comment explains _why_ the bitbang variant exists
  (MK10 has no second hardware I²C peripheral available on the external
  bus pins) and why it lives in this file rather than as a generic
  transport (scoped fix for one board generation; MK11+ does not need it).

### The bitbang transport

Implementation details for the three transport primitives in
`SpadefootToadSensorWithBitbangI2C`:

- **Open-drain emulation.** SDA and SCL are configured as
  `GPIO_MODE_INPUT_OUTPUT_OD`. To drive a line low, set the level to 0. To
  release, set the level to 1 and let the external pullup pull it high. The
  internal ~45 kΩ pullup is enabled as a safety net; **external pullups on
  the MK10 schematic are mandatory** for clean edges over the ~1 m cable.
  Verify the schematic has them before writing software.
- **Bit timing.** Fixed at 100 kHz to match the Spadefoot Toad's TWI Address
  Match wakeup requirement (5 µs half-bit). `esp_rom_delay_us(5)` per
  half-bit. Cached as an integer; no floating-point on the hot path.
- **Clock stretching.** After releasing SCL, poll it in a tight loop until
  the slave releases it (or a ~10 ms timeout fires, after which we abort
  the transaction). The Spadefoot Toad explicitly stretches during TWI
  Address Match wakeup, so this is not optional.
- **Critical sections.** `portENTER_CRITICAL` around each byte (eight bits +
  ACK), not around an entire transaction. Per-byte keeps the worst-case
  ISR-disabled window under ~90 µs at 100 kHz, which WiFi can tolerate;
  per-transaction would risk hundreds of microseconds and trigger WDT
  scolding under WiFi load.
- **State.** Two `InternalPinPtr` members for SDA/SCL, plus a `Mutex` for
  concurrent caller serialization (each public method takes the mutex
  before issuing a transaction).

### Settings and factory

A new factory function, `makeFactoryForSpadefootToadSensorWithBitbangI2C()`,
registered as the peripheral type `soil:spadefoot-toad-bb`. The settings
schema reuses `SpadefootToadSensorSettings` — both flavors take `sda`,
`scl`, optional `address`, `logRawValues`.

The factory differs from the hardware one in exactly two places:

- It does **not** consult `params.services.i2c`. The bitbang sensor is
  constructed with raw pin pointers.
- It pins the clock to 100 kHz unconditionally (the bitbang transport
  rejects anything else).

### Registration

`DeviceDefinition::registerPeripheralFactories` continues to register the
hardware-I²C factory under `soil:spadefoot-toad` for every device.

The bitbang factory is registered **only** from MK10's
`registerDeviceSpecificPeripheralFactories` in
`components/devices/src/devices/UglyDucklingMk10.hpp`. The override carries
a comment explaining:

- Why it is MK10-only (no second HP_I2C peripheral on this board, and we
  deliberately did not write a general bitbang transport for one board
  generation).
- That MK11+ does not register the bitbang variant — its external bus is
  back on HP_I2C0 and every standard driver works there.
- That if a future MK10 firmware ever needs a second non-Spadefoot
  external I²C device, the answer is to revisit the general-bitbang
  question, not to copy this driver.

### Config template

`config-templates/plot-controller-mk10.json` switches the soil entry to:

```json
{
  "name": "soil",
  "type": "soil:spadefoot-toad-bb",
  "params": {
    "sda": "EXT_SDA",
    "scl": "EXT_SCL"
  }
}
```

Internal-bus peripherals (the `environment:sht3x` air sensor in the template
today, plus any future additions) continue to use the unsuffixed names and
the `SDA` / `SCL` pin aliases.

### Testing

- **Unit (`test/unit-tests/`):** the bitbang state machine behind an
  `IGpio`-like interface — START / STOP / ACK / NACK / stretch — mocked in
  native Catch2.
- **Wokwi (`test/embedded-tests/`):** an MK10-targeted diagram with a
  Spadefoot Toad slave. Boot, verify the MFR_ID / device-ID handshake, run a
  full measurement cycle, read calibration EEPROM.
- **Hardware bring-up:** flash an MK10 with a real Spadefoot Toad on the
  external connector. Confirm `0x434D` from `CMD_GET_MFR_ID`. Scope SDA and
  SCL for edge cleanliness, especially the released-high rise time
  (sets the worst-case pullup we can tolerate). Run the measurement loop
  under WiFi load — any checksum mismatches mean we need to revisit the
  per-byte critical section sizing.

### Implementation checklist

- [ ] Extract shared protocol code from `SpadefootToadSensor` into a
      private base class `SpadefootToadSensorBase` in the same header.
- [ ] Refactor `SpadefootToadSensor` to subclass `SpadefootToadSensorBase`
      and forward the three transport primitives to `I2CDevice`.
- [ ] Add `SpadefootToadSensorWithBitbangI2C` with the inlined bitbang
      transport (open-drain, 100 kHz, clock stretching, per-byte critical
      sections).
- [ ] Add `makeFactoryForSpadefootToadSensorWithBitbangI2C()` exporting
      peripheral type `soil:spadefoot-toad-bb`.
- [ ] Register the bitbang factory from
      `UglyDucklingMk10::registerDeviceSpecificPeripheralFactories` with
      explanatory comment.
- [ ] Add a file-level comment to `SpadefootToadSensor.hpp` explaining the
      two variants and the MK10 constraint.
- [ ] Update `config-templates/plot-controller-mk10.json` to use
      `soil:spadefoot-toad-bb` with `EXT_SDA` / `EXT_SCL`.
- [ ] Confirm external pullups exist on the MK10 schematic.
- [ ] Unit tests for the bitbang state machine in `test/unit-tests/`.
- [ ] Wokwi diagram + embedded test fixture for an MK10 with a Spadefoot
      Toad slave.
- [ ] Hardware bring-up on MK10: MFR_ID handshake, full measurement loop
      under WiFi load, scope SDA/SCL rise times.

## MK11+ plan: internal bus on LP_I2C0

For next-generation hardware (MK11 and onward), route SDA/SCL of the
**internal** bus to `GPIO_NUM_6` / `GPIO_NUM_7`, the pins LP_I2C0 is locked
to. Keep the external bus on GPIO10/11 (HP_I2C0). The mapping then looks
like:

| Bus      | Pins            | ESP-IDF port          | Devices                           |
| -------- | --------------- | --------------------- | --------------------------------- |
| Internal | GPIO6 / GPIO7   | `LP_I2C_NUM_0`        | `BQ27220`, `INA219`               |
| External | GPIO10 / GPIO11 | `I2C_NUM_0` (HP_I2C0) | Any I²C peripheral driver we have |

### LP_I2C from HP code

LP*I2C0 is operated from HP code through the **same** `i2c_master*\*`API as
HP_I2C0. Confirmed by the IDF test`components/esp_driver_i2c/test_apps/i2c_test_apps/main/test_lp_i2c.cpp`:

```cpp
i2c_master_bus_config_t i2c_mst_config = {};
i2c_mst_config.lp_source_clk = LP_I2C_SCLK_DEFAULT;
i2c_mst_config.i2c_port = LP_I2C_NUM_0;
i2c_mst_config.scl_io_num = LP_I2C_SCL_IO;   // GPIO_NUM_7
i2c_mst_config.sda_io_num = LP_I2C_SDA_IO;   // GPIO_NUM_6
i2c_mst_config.flags.enable_internal_pullup = true;

i2c_master_bus_handle_t bus_handle;
i2c_new_master_bus(&i2c_mst_config, &bus_handle);
// i2c_master_bus_add_device, i2c_master_transmit_receive, … — identical API.
```

Per the ESP32-C6 TRM §29.5, LP_I2C "includes all the functions of the
ESP32-C6 I²C master" — synchronous transactions
(`i2c_master_transmit`, `i2c_master_receive`,
`i2c_master_transmit_receive` with repeated START) all work the same way.

The only differences that touch us:

- **`lp_source_clk` field** (e.g. `LP_I2C_SCLK_DEFAULT` selecting CLK_AON_FAST
  / RTC_FAST) instead of `clk_source = I2C_CLK_SRC_DEFAULT`. Passing the HP
  clock source returns `ESP_ERR_NOT_SUPPORTED`.
- **16-byte TX/RX FIFO** instead of 32. Comfortably above what either
  internal device asks for (BQ27220 transactions are ≤4 bytes; INA219 is
  2-byte registers).
- **No slave mode** — irrelevant, we are master only.
- **No async / multi-buffer driver APIs** (e.g.
  `i2c_master_transmit_multi_buffer`). We do not use these today.

We are explicitly **not** pursuing LP-core-only operation of LP*I2C0 (the
sleep-survival use case via `ulp_lp_core_i2c_master*\*`). Polling battery
state during sleep is not on the product roadmap, and adopting that path
would mean an LP-core firmware that wraps the I²C transactions and parks
results in RTC slow memory — far more work than the gain.

### Code changes for MK11+

Two narrow changes in `components/kernel/src/I2CManager.hpp`:

1. **Pin-aware port allocation.** `getBusFor(sda, scl)` currently assigns
   ports in registration order. On ESP32-C6, allocate `LP_I2C_NUM_0` when
   the requested pair is `(GPIO_NUM_6, GPIO_NUM_7)`; otherwise pick the
   next free HP port (just `I2C_NUM_0`, since C6 has only one HP I²C). On
   ESP32-S3, behavior is unchanged.

2. **Per-port clock-source.** When installing the bus, use
   `lp_source_clk = LP_I2C_SCLK_DEFAULT` for the LP port and
   `clk_source = I2C_CLK_SRC_DEFAULT` for HP ports.

### Working around `i2cdev`'s clock-source assumption

`managed_components/esp-idf-lib__i2cdev/i2cdev.c:426` hard-codes
`clk_source = I2C_CLK_SRC_DEFAULT` on its `i2c_new_master_bus` call. The IDF
test above proves that returns `ESP_ERR_NOT_SUPPORTED` for LP_I2C, so we
cannot let `i2cdev` be the first thing to install the LP_I2C bus.

We already use the inverse handle-reuse pattern today in `Bq27220Driver`
(see the comment at `components/kernel/src/drivers/Bq27220Driver.hpp:81–86`):
`espressif/i2c_bus` — which BQ27220 sits on — calls
`i2c_master_get_bus_handle()` first and only calls `i2c_new_master_bus()` if
no bus exists. For MK11+ we apply the same pattern, just flipped:

- `I2CManager` installs the LP_I2C bus itself with the correct
  `lp_source_clk` **before** any `i2cdev`-using driver touches the port.
- `espressif/bq27220` finds the existing handle via
  `i2c_master_get_bus_handle()` and reuses it — the BQ27220 path "just
  works."

There is one wrinkle: `esp-idf-lib/i2cdev` does _not_ do a handle-reuse
lookup before its `i2c_new_master_bus` call. If an `i2cdev`-based driver
later targets the pre-installed LP_I2C port, the second install fails and
`i2cdev` marks the port unusable. `Ina219Driver` is the only such driver on
the internal bus today. The narrowest fix is to give `Ina219Driver` the same
"reuse if present" treatment BQ27220 already gets — either by accepting a
preinstalled `i2c_master_bus_handle_t` directly, or by routing it through
our `I2CDevice` abstraction. The decision can wait until MK11+ bring-up;
it is a one-driver problem, not a tree-wide one.

### What does _not_ change

- `Bq27220Driver`, `SpadefootToadSensor`, and all `esp-idf-lib`-based
  environment / light sensors are untouched. They go through `I2CDevice` or
  hand a port number to a library that reuses the bus handle, and the
  dispatch happens below them.
- `UglyDucklingMk10.hpp` is unaffected. MK10 keeps using HP_I2C0 on
  GPIO2/3 with the bitbang Spadefoot on the external bus.

### Implementation checklist

- [ ] Pin-aware port assignment in `I2CManager::getBusFor` for ESP32-C6
      (LP_I2C_NUM_0 for `(GPIO_NUM_6, GPIO_NUM_7)`, HP otherwise).
- [ ] Per-port clock-source selection (`lp_source_clk` for LP_I2C,
      `clk_source` for HP) on bus install.
- [ ] Pre-install the LP_I2C bus in `I2CManager` before any `i2cdev`
      consumer touches the port.
- [ ] Accommodate `Ina219Driver` so it reuses the existing bus handle
      instead of going through `i2cdev`'s install path (or accept a
      preinstalled handle).
- [ ] New `UglyDucklingMk11.hpp` device definition (mirrors MK10 with
      GPIO6/7 swapped to the internal bus).
- [ ] Hardware bring-up on MK11+: BQ27220 and INA219 over LP_I2C0,
      pluggable peripherals over HP_I2C0.

## Out of scope

- **General-purpose bitbang transport.** Considered and rejected for this
  cycle. MK10's external bus serves the Spadefoot Toad only; if a future
  MK10 firmware needs a second non-Spadefoot external device, revisit
  this decision.
- **LP-core operation of LP_I2C0.** Sleep-survival polling of BQ27220 is
  not on the product roadmap. The MK11+ plan uses LP_I2C0 from HP code only.
- **Sleep retention of the MK10 bitbang bus.** The bitbang transport
  requires the CPU; during light sleep no transactions can occur on it.
  This matches current behavior for the hardware bus too, so no
  regression.
- **Multi-master arbitration.** Both buses on both boards are
  single-master.
