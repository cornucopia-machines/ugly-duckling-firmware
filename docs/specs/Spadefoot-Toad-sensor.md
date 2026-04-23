# Spadefoot Toad Soil Sensor Implementation Plan

## Overview

Add support for the [Spadefoot Toad](https://github.com/cornucopia-machines/spadefoot-toad)
capacitive soil sensor. The device is an ATtiny1616-based I2C slave that measures soil moisture and
temperature at two depths (5 cm and 15 cm).

Hardware layout:

- **Four moisture electrode pairs**: TF (top-front), TR (top-rear), BF (bottom-front), BR (bottom-rear)
- **Two NTC thermistors**: one at the top zone, one at the bottom zone

We report:

- One `"moisture"` feature — the mean of however many of the four moisture readings are valid (0xFF means
  invalid/uncalibrated; exclude those from the average). Returns NaN if no valid reading exists.
- One `"temperature"` feature — the mean of the two temperature readings, in °C. Returns NaN if the
  temperature valid flag is not set or either reading is individually invalid.

Calibration (CALIBRATE_DRY / CALIBRATE_WET) is permanently out of scope for this driver: calibration
is performed by the manufacturer before the sensor ships. The driver never calibrates or reads raw values.

## Device identifiers

These are verified once during initialization. Log a warning and abort if any mismatches.

| Command | Expected response | Meaning |
| ------- | ----------------- | ------- |
| `GET_MFR_ID` (`0xFE`) | `0x434D` (`"CM"` big-endian) | Manufacturer: Cornucopia Machines |
| `GET_DEVICE_ID` (`0xFF`) | `0x0010` | Spadefoot Toad |
| `GET_FIRMWARE_REV` (`0xFC`) | any `uint16_t` | Log at INFO level |
| `GET_DEVICE_REV` (`0xFD`) | any `uint16_t` | Log at INFO level |

All four are single-byte write transactions immediately followed by a two-byte read:

```text
master → [cmd_byte]       STOP
master → repeated START (read direction)
slave  → [MSB] [LSB]      STOP
```

## I2C constraints

- **Speed: 100 kHz only.** The ATtiny TWI Address Match wakeup is incompatible with 400 kHz. This is a
  hard constraint imposed by the device firmware — do not allow the caller to change it.
- **Default address: `0x20`**, configurable via `SET_ADDRESS` (out of scope here).

## Two-phase measurement protocol

The sensor uses a two-phase protocol to avoid I2C timeouts during measurement:

**Phase 1 — TRIGGER (`0x01`):** a write-only transaction. The ATtiny wakes, takes all
measurements, stores results in RAM, and returns to sleep. No read follows.

**Phase 2 — READ (`0x02`):** send the command byte, then read the 10-byte response. Must be
issued at least 1 second after TRIGGER.

```text
Phase 1:   master → [0x01]  STOP
           (wait ≥ 1 s)
Phase 2:   master → [0x02]  STOP
           master → (read 10 bytes)  STOP
```

`probe.py` waits 2 seconds between phases; 1 second is the documented minimum. Use 1 second in
`DebouncedMeasurement` (the timer-based re-measurement already provides natural separation).

## READ response packet (`0x02`) — 10 bytes

```c
struct SoilResponse {
    uint8_t  moisture[4];  // [0]=TF, [1]=TR, [2]=BF, [3]=BR
                           // 0–100 = percentage; 0xFF = invalid (not calibrated or timeout)
    int16_t  temp_top;     // big-endian; units of 0.1 °C; INT16_MIN = invalid
    int16_t  temp_bot;     // big-endian; units of 0.1 °C; INT16_MIN = invalid
    uint8_t  flags;        // bit 0 = moisture valid, bit 1 = temperature valid
    uint8_t  checksum;     // XOR of bytes [0..8]
};
```

### Checksum

XOR all nine bytes before the checksum byte. If the result does not equal `checksum`, discard the
reading entirely and return NaN for both features.

### Flags

- **bit 0 (`MOISTURE_VALID`)**: if clear, all `moisture[]` bytes should be treated as invalid
  regardless of their value.
- **bit 1 (`TEMPERATURE_VALID`)**: if clear, both temperature readings are invalid and NaN is
  reported for the temperature feature.

Individual moisture entries with value `0xFF` are also invalid even when bit 0 is set (can happen
when only some probes time out). Exclude them from the average.

## Initialization sequence

Performed once in the constructor, before the measurement loop starts:

1. Write `GET_MFR_ID`, read 2 bytes → compare to `0x434D`. If mismatch: log error, throw exception.
2. Write `GET_DEVICE_ID`, read 2 bytes → compare to `0x0010`. If mismatch: log error, throw exception.
3. Write `GET_FIRMWARE_REV`, read 2 bytes → log as `"Spadefoot Toad firmware rev: 0x%04X"`.
4. Write `GET_DEVICE_REV`, read 2 bytes → log as `"Spadefoot Toad device rev: 0x%04X"`.

## Class design

Follow the `ChirpSoilSensor` pattern closely: a single class implementing both
`api::ISoilMoistureSensor` and `api::ITemperatureSensor`, backed by `DebouncedMeasurement`.

New file: `components/peripherals/src/peripherals/environment/SpadefootToadSensor.hpp`

```cpp
class SpadefootToadSensorSettings
    : public I2CSettings {
    // No extra fields for now; I2C address comes from I2CSettings.
};

struct SpadefootToadReading {
    double moisture    = std::numeric_limits<double>::quiet_NaN();
    double temperature = std::numeric_limits<double>::quiet_NaN();
};

class SpadefootToadSensor final
    : public api::ISoilMoistureSensor,
      public api::ITemperatureSensor,
      public Peripheral {
public:
    SpadefootToadSensor(
        const std::string& name,
        const std::shared_ptr<I2CManager>& i2c,
        const I2CConfig& config);

    Percent getMoisture() override;
    Celsius getTemperature() override;

private:
    static constexpr uint8_t CMD_TRIGGER          = 0x01;
    static constexpr uint8_t CMD_READ             = 0x02;
    static constexpr uint8_t CMD_GET_FIRMWARE_REV = 0xFC;
    static constexpr uint8_t CMD_GET_DEVICE_REV   = 0xFD;
    static constexpr uint8_t CMD_GET_MFR_ID       = 0xFE;
    static constexpr uint8_t CMD_GET_DEVICE_ID    = 0xFF;

    static constexpr uint16_t EXPECTED_MFR_ID    = 0x434D;
    static constexpr uint16_t EXPECTED_DEVICE_ID = 0x0010;

    static constexpr uint8_t  FLAG_MOISTURE_VALID     = 0x01;
    static constexpr uint8_t  FLAG_TEMPERATURE_VALID  = 0x02;
    static constexpr uint8_t  MOISTURE_INVALID       = 0xFF;
    static constexpr int16_t  TEMP_INVALID           = INT16_MIN;

    const std::shared_ptr<I2CDevice> device;

    utils::DebouncedMeasurement<SpadefootToadReading> measurement { /* see below */ };
};
```

### Measurement lambda

```cpp
[this](const utils::DebouncedParams<SpadefootToadReading>) -> std::optional<SpadefootToadReading> {
    // Phase 1: trigger
    device->writeByte(CMD_TRIGGER);

    // Phase 2: wait 1 s, then read
    Task::delay(1s);
    auto raw = device->readBytes(CMD_READ, 10);  // write CMD_READ, then read 10 bytes

    // Validate checksum
    uint8_t csum = 0;
    for (int i = 0; i < 9; i++) csum ^= raw[i];
    if (csum != raw[9]) {
        LOGTW(ENV, "Spadefoot Toad '%s': checksum mismatch", name.c_str());
        return std::nullopt;
    }

    SpadefootToadReading reading;
    uint8_t flags = raw[8];

    // Moisture: average of valid probes (flag bit 0 set and value != 0xFF)
    if (flags & FLAG_MOISTURE_VALID) {
        int sum = 0, count = 0;
        for (int i = 0; i < 4; i++) {
            if (raw[i] != MOISTURE_INVALID) {
                sum += raw[i];
                count++;
            }
        }
        if (count > 0) {
            reading.moisture = static_cast<double>(sum) / count;
        }
        LOGTV(ENV, "Spadefoot Toad '%s': moisture TF=%d TR=%d BF=%d BR=%d → avg=%.1f%%",
            name.c_str(), raw[0], raw[1], raw[2], raw[3], reading.moisture);
    }

    // Temperature: average of top and bottom.
    // Requires the temperature valid flag AND both individual readings to be valid.
    if (flags & FLAG_TEMPERATURE_VALID) {
        auto temp_top = static_cast<int16_t>((raw[4] << 8) | raw[5]);
        auto temp_bot = static_cast<int16_t>((raw[6] << 8) | raw[7]);
        if (temp_top != TEMP_INVALID && temp_bot != TEMP_INVALID) {
            reading.temperature = (temp_top + temp_bot) / 20.0;  // two sensors, 0.1 °C units
            LOGTV(ENV, "Spadefoot Toad '%s': temp top=%.1f°C bot=%.1f°C → avg=%.1f°C",
                name.c_str(), temp_top / 10.0, temp_bot / 10.0, reading.temperature);
        } else {
            LOGTW(ENV, "Spadefoot Toad '%s': temperature valid flag set but individual reading(s) invalid (top=%d bot=%d)",
                name.c_str(), temp_top, temp_bot);
        }
    }

    return reading;
}
```

The debounce interval is **2 seconds** — enough to cover the 1-second TRIGGER-to-READ wait plus
measurement overhead.

### I2C API additions

The `I2CDevice` API currently exposes `readRegWord()` for 2-byte register reads. The Spadefoot Toad
needs two additional methods; add them to `I2CDevice` if not already present:

- **`writeByte(uint8_t cmd)`**: a write-only transaction — `[cmd] STOP` — with no subsequent read.
  Used for TRIGGER.
- **`readBytes(uint8_t cmd, size_t n)`**: write `[cmd]`, then read `n` bytes. Used for READ (10
  bytes) and GET_* commands (2 bytes each). Whether this uses a repeated-start or a full
  stop-start between the write and read is an implementation detail; the ATtiny should handle
  both. Verify on real hardware during bringup.

## Factory function

```cpp
inline PeripheralFactory makeFactoryForSpadefootToadSensor() {
    return makePeripheralFactory<
        SpadefootToadSensor,
        SpadefootToadSensorSettings,
        api::ISoilMoistureSensor,
        api::ITemperatureSensor>(
        "soil:spadefoot-toad",
        "environment",
        [](PeripheralInitParameters& params, const std::shared_ptr<SpadefootToadSensorSettings>& settings) {
            I2CConfig i2cConfig = settings->parse(0x20);
            auto sensor = std::make_shared<SpadefootToadSensor>(
                params.name,
                params.services.i2c,
                i2cConfig);
            params.registerFeature("moisture", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getMoisture();
            });
            params.registerFeature("temperature", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getTemperature();
            });
            return sensor;
        });
}
```

## Registering the factory

In `DeviceDefinition.hpp` (or whichever device header is appropriate):

```cpp
#include <peripherals/environment/SpadefootToadSensor.hpp>
// ...
peripheralManager->registerFactory(environment::makeFactoryForSpadefootToadSensor());
```

## JSON configuration example

```jsonc
{
  "type": "soil:spadefoot-toad",
  "name": "bed-soil",
  "params": {
    "address": "0x20",
    "sda": "GPIO4",
    "scl": "GPIO5"
  }
}
```

## Open questions

1. **I2C clock speed**: confirm the `I2CConfig`/`I2CManager` path that enforces 100 kHz. If the bus
   is shared with other peripherals running at 400 kHz, the Spadefoot Toad will need its own
   dedicated I2C bus.
2. **Stop-vs-repeated-start for `readBytes`**: the `probe.py` reference tool uses a full stop
   between the command write and the data read. Verify that the ATtiny handles repeated-start
   equally well during bringup, in case the ESP-IDF I2C driver combines them.
