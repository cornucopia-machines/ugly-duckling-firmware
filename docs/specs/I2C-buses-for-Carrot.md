# External I2C for MK10 on ESP32-C6 (Carrot)

## Status

The MK10 software plan is **implemented** (branch `hardware/mk10-hw`). The
remaining checklist items are hardware validation: confirming external pullups
on the schematic, unit tests for the bitbang state machine, a Wokwi embedded
test, and physical bring-up on an MK10 board.

The MK11+ plan is **not yet started** — it depends on MK11+ hardware existing.

### What was done

`SpadefootToadSensor.hpp` was refactored into three classes sharing a common
base (`SpadefootToadSensorBase`) that holds all protocol logic and exposes
three pure-virtual transport primitives (`transportWriteByte`,
`transportReadWord`, `transportReadBytes`):

- **`SpadefootToadSensor`** (unchanged externally) — thin subclass forwarding
  the three primitives to an `I2CDevice`. Spinach / MK11+ path is unaffected.
- **`SpadefootToadSensorWithBitbangI2C`** (new, MK10-only) — implements the
  same three primitives using open-drain GPIO bitbang at 100 kHz, with clock
  stretching (10 ms timeout) and per-byte `portENTER_CRITICAL` sections
  (≤90 µs ISR-disabled window). A `Mutex` serializes concurrent callers.

`UglyDucklingMk10::registerDeviceSpecificPeripheralFactories` now registers
`makeFactoryForSpadefootToadSensorWithBitbangI2C()` (type
`soil:spadefoot-toad-bb`) with a comment explaining the MK10 constraint and
discouraging copying this pattern for future devices.

`config-templates/plot-controller-mk10.json` was updated to include the soil
sensor entry (`soil:spadefoot-toad-bb`, pins `EXT_SDA` / `EXT_SCL`).

Both ESP32-C6 (MK10/Carrot) and ESP32-S3 (Spinach) builds pass cleanly.

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
  Confirmed: 2.2 kΩ pullups are present on both SDA and SCL.
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

- [x] Extract shared protocol code from `SpadefootToadSensor` into a
      private base class `SpadefootToadSensorBase` in the same header.
- [x] Refactor `SpadefootToadSensor` to subclass `SpadefootToadSensorBase`
      and forward the three transport primitives to `I2CDevice`.
- [x] Add `SpadefootToadSensorWithBitbangI2C` with the inlined bitbang
      transport (open-drain, 100 kHz, clock stretching, per-byte critical
      sections).
- [x] Add `makeFactoryForSpadefootToadSensorWithBitbangI2C()` exporting
      peripheral type `soil:spadefoot-toad-bb`.
- [x] Register the bitbang factory from
      `UglyDucklingMk10::registerDeviceSpecificPeripheralFactories` with
      explanatory comment.
- [x] Add a file-level comment to `SpadefootToadSensor.hpp` explaining the
      two variants and the MK10 constraint.
- [x] Update `config-templates/plot-controller-mk10.json` to use
      `soil:spadefoot-toad-bb` with `EXT_SDA` / `EXT_SCL`.
- [x] Confirm external pullups exist on the MK10 schematic. (2.2 kΩ on both SDA and SCL.)
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

### i2cdev LP_I2C limitation and workaround

`i2cdev.c` hardcodes `.clk_source = I2C_CLK_SRC_DEFAULT` when calling
`i2c_new_master_bus`. For `LP_I2C_NUM_0` this is the wrong field — LP_I2C
requires `.lp_source_clk` — so i2cdev fails with `ESP_ERR_NOT_SUPPORTED` when
asked to install the LP bus on its own.

`I2CManager::preInstallIfLp` side-steps this by calling `i2c_new_master_bus`
directly with `lp_source_clk = LP_I2C_SCLK_DEFAULT` the first time `getBusFor`
sees `LP_I2C_NUM_0`. This is already implemented and the bus installs correctly.
`espressif/bq27220`'s data-path operations are unaffected: `espressif/i2c_bus`
calls `i2c_master_get_bus_handle()` first and reuses the pre-installed handle.

However, i2cdev's internal `port_state->installed` flag is never set by the
pre-install, so any i2cdev device later targeting the LP port will attempt a
second `i2c_new_master_bus` and fail with `ESP_ERR_INVALID_STATE`. The drivers
currently affected are `Bq27220Driver` (via `I2CDevice::probeRead()`) and
`Ina219Driver` (uses i2cdev directly).

The root fix belongs upstream — see [Long-term i2cdev strategy](#long-term-i2cdev-strategy).
Once i2cdev is LP-aware, `preInstallIfLp` is removed and every i2cdev-based
driver (including INA219) works on LP_I2C without modification.

### What does _not_ change

- `Bq27220Driver`, `SpadefootToadSensor`, and all `esp-idf-lib`-based
  environment / light sensors are untouched. They go through `I2CDevice` or
  hand a port number to a library that reuses the bus handle, and the
  dispatch happens below them.
- `UglyDucklingMk10.hpp` is unaffected. MK10 keeps using HP_I2C0 on
  GPIO2/3 with the bitbang Spadefoot on the external bus.

### Implementation checklist

- [x] Pin-aware port assignment in `I2CManager::getBusFor` for ESP32-C6
      (LP_I2C_NUM_0 for `(GPIO_NUM_6, GPIO_NUM_7)`, I2C_NUM_0 otherwise).
- [x] Per-port clock-source selection (`lp_source_clk` for LP_I2C,
      `clk_source` for HP) on bus install.
- [x] Pre-install the LP_I2C bus in `I2CManager` before any `i2cdev`
      consumer touches the port.
- [ ] Switch i2cdev dependency to the fork with LP-awareness fix; remove
      `preInstallIfLp` from `I2CManager`. Until this lands, `Bq27220Driver`
      probeRead and `Ina219Driver` cannot run on LP_I2C. See
      [Long-term i2cdev strategy](#long-term-i2cdev-strategy).
- [ ] New `UglyDucklingMk11.hpp` device definition (mirrors MK10 with
      GPIO6/7 swapped to the internal bus).
- [ ] Hardware bring-up on MK11+: BQ27220 and INA219 over LP_I2C0,
      pluggable peripherals over HP_I2C0.

## Long-term i2cdev strategy

`preInstallIfLp` and the remaining LP_I2C limitations both stem from the same
root cause: `esp-idf-lib/i2cdev` hardcodes `.clk_source = I2C_CLK_SRC_DEFAULT`
in `i2c_new_master_bus`, but `LP_I2C_NUM_0` requires `.lp_source_clk` instead.
This is a bug — any project using i2cdev with `LP_I2C_NUM_0` silently fails.

### Fix plan

Three issues/PRs against `https://github.com/esp-idf-lib/i2cdev`:

**1. LP_I2C support (bug fix — file as PR)**

In `i2c_setup_port`, detect `LP_I2C_NUM_0` and build the bus config with
`.lp_source_clk = LP_I2C_SCLK_DEFAULT` instead of
`.clk_source = I2C_CLK_SRC_DEFAULT`. Guard with `#ifdef LP_I2C_NUM_0` so the
change is a no-op on chips without LP_I2C. The `clk_flags` field already in
`i2c_dev_t` is currently dead code and could be wired through here.

This is a ~15-line change. Once active in our fork and the dependency updated,
`preInstallIfLp` is removed from `I2CManager`, and every i2cdev-based driver —
including `Ina219Driver` — works on LP_I2C without modification.

**2. Externally pre-installed bus handle adoption (enhancement — file as issue)**

i2cdev should call `i2c_master_get_bus_handle` before `i2c_new_master_bus` and
silently adopt an existing handle rather than failing with
`ESP_ERR_INVALID_STATE`. This is the same pattern `espressif/i2c_bus` already
implements. It makes i2cdev robust when other components (e.g. `esp_lcd`) or
application code pre-installs a bus on the same port.

Not required for our use case once fix 1 lands (since `preInstallIfLp` is
removed), but useful defensive behaviour for the wider ecosystem. File with a
sketch of the `externally_owned` flag approach and the `espressif/i2c_bus`
prior art.

**3. Port pre-registration API (enhancement — file as issue)**

An `i2cdev_open_port(port, sda, scl)` function that installs the bus without
requiring a dummy device. Useful for ensuring LP_I2C (or any bus) is installed
at a well-defined point in startup rather than lazily on first transaction. Can
be filed together with or as a follow-on to issue 2.

### Interim approach

Fork `esp-idf-lib/i2cdev`, apply fix 1, and point the project at the fork via
a git reference in `idf_component.yml` while the upstream PR is in review. This
unblocks MK11+ bring-up without waiting for a merge.

### Effect on our codebase once fix 1 is active

- `preInstallIfLp` removed from `I2CManager`.
- No `I2CDevice` LP bypass needed — i2cdev installs LP_I2C correctly on first use.
- `Ina219Driver` LP limitation removed — works on LP_I2C like any other i2cdev driver.
- `i2cdev_init` / `i2cdev_done` calls in `I2CManager` unchanged.

### Option B: Migrate off i2cdev entirely

If the PR does not land in time for MK11+ bring-up, or if the esp-idf-lib
organisation declines the change, the alternative is to replace each
i2cdev-based driver with an implementation that uses the new `i2c_master`
API directly (through `I2CDevice`).

| Chip | Current dep | Replacement path |
| ---- | ----------- | ---------------- |
| SHT3x | `esp-idf-lib/sht3x` | `espressif/sht3x` v0.2.0 — Espressif-official, wraps `espressif/i2c_bus` which does handle-reuse exactly like bq27220. Dependency swap only, no code changes. |
| SI7021 / SHT2x | `esp-idf-lib/si7021` | Port required. No maintained `i2c_master` alternative exists. The protocol is simple (~50 lines of I2C transactions). |
| BH1750 | `esp-idf-lib/bh1750` | `k0i05/esp_bh1750` v1.2.7 on the component registry — raw `i2c_master` API, actively maintained, MIT. |
| TSL2591 | `esp-idf-lib/tsl2591` | Port required from the existing MIT-licensed source (~200 lines of register-map I2C code). No new-API alternative found. |
| INA219 | `esp-idf-lib/ina219` | Port required. `fborello-lambda/solar_panel_curve_tracer` on GitHub is a clean, complete reference using the new API. |

The two drop-in swaps (SHT3x, BH1750) require no I2C-level changes; the three
ports (SI7021, TSL2591, INA219) require implementing protocol logic against
`I2CDevice`. End state: no i2cdev dependency, `I2CDevice` uses the new
`i2c_master` API directly, no `#if` guards.

### Which to pursue

Apply fix 1 to the fork first — the code change is small, clearly a bug, and
unblocks MK11+ bring-up regardless of upstream merge timeline. File issue 2
(handle-adoption) separately so the upstream maintainers can evaluate it
independently of the bug fix. If fix 1 merges upstream, Option B reduces to the
three ports with no ready-made drop-in (SI7021, TSL2591, INA219), which can be
ported lazily. If upstream is unresponsive, stay on the fork until a decision
is made on Option B.

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
