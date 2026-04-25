# Spadefoot Toad: Calibration Commands

## Overview

Add three admin-facing MQTT commands to the Spadefoot Toad driver, enabling factory calibration
from the field without a dedicated programmer tool:

| Command            | Payload                           | Response              |
| ------------------ | --------------------------------- | --------------------- |
| `calibrate`        | `{ "reference": "dry" \| "wet" }` | _(none)_              |
| `read-calibration` | _(none)_                          | calibration data JSON |
| `factory-reset`    | _(none)_                          | _(none)_              |

Commands arrive via MQTT at `devices/<device-id>/peripherals/<name>/commands/<command>` and are
dispatched through the existing `MqttRoot` command-handler mechanism.

## Infrastructure: peripheral command registration

### 1. Add `mqttRoot` to `PeripheralServices`

`PeripheralServices` (in `Peripheral.hpp`) gains one new field:

```cpp
struct PeripheralServices {
    // ... existing fields ...
    const std::shared_ptr<mqtt::MqttRoot> mqttRoot;
};
```

In `Device.hpp`, populate it when constructing `PeripheralServices`:

```cpp
auto peripheralServices = PeripheralServices {
    // ... existing fields ...
    .mqttRoot = mqttRoot,
};
```

### 2. Extend `PeripheralInitParameters`

Add a `registerCommand()` method. It lazily constructs a peripheral-scoped `MqttRoot` under
`peripherals/<name>` the first time a command is registered, then delegates to it:

```cpp
struct PeripheralInitParameters {
    // ... existing public members and registerFeature() ...

    void registerCommand(const std::string& command, const mqtt::CommandHandler& handler) {
        if (!peripheralMqttRoot) {
            peripheralMqttRoot = services.mqttRoot->forSuffix("peripherals/" + name);
        }
        peripheralMqttRoot->registerCommand(command, handler);
    }

    // ... existing fields ...

private:
    std::shared_ptr<mqtt::MqttRoot> peripheralMqttRoot;   // null until first registerCommand()
};
```

`services.mqttRoot` is the device-level `MqttRoot` already stored in `PeripheralServices`.
No change is needed in `PeripheralManager::createPeripheral` — `services` is already forwarded.

#### MQTT topic layout

```text
devices/<device-id>/peripherals/<name>/commands/<command>   ← incoming request
devices/<device-id>/peripherals/<name>/responses/<command>  ← response (if non-empty)
```

These are the standard topic patterns established by `MqttRoot::registerCommand()` and
`MqttRoot::forSuffix()` — no new convention is introduced.

### 3. Header includes

`Peripheral.hpp` will need:

```cpp
#include <mqtt/MqttRoot.hpp>
```

## `CalibrationReference` enum

Introduce a scoped enum in the `environment` namespace (same file as `SpadefootToadSensor`):

```cpp
enum class CalibrationReference : uint8_t {
    Dry,
    Wet,
};
```

Provide ArduinoJSON string serialization in the `ArduinoJson` namespace (after the class
definition, following the same pattern used by `OperationState` in `Door.hpp`):

```cpp
namespace ArduinoJson {

using cornucopia::ugly_duckling::peripherals::environment::CalibrationReference;

template <>
struct Converter<CalibrationReference> {
    static bool toJson(const CalibrationReference& src, JsonVariant dst) {
        switch (src) {
            case CalibrationReference::Dry: return dst.set("dry");
            case CalibrationReference::Wet: return dst.set("wet");
        }
        return false;
    }

    static CalibrationReference fromJson(JsonVariantConst src) {
        std::string s = src.as<std::string>();
        if (s == "wet") return CalibrationReference::Wet;
        return CalibrationReference::Dry;    // default / unknown → Dry
    }

    static bool checkJson(JsonVariantConst src) {
        if (!src.is<const char*>()) return false;
        std::string s = src.as<std::string>();
        return s == "dry" || s == "wet";
    }
};

}    // namespace ArduinoJson
```

## Spadefoot Toad hardware protocol (calibration commands)

| Command            | Byte   | Direction  | Notes                                                        |
| ------------------ | ------ | ---------- | ------------------------------------------------------------ |
| `CALIBRATE_DRY`    | `0x04` | write-only | Takes an immediate internal measurement; stores to EEPROM    |
| `CALIBRATE_WET`    | `0x05` | write-only | Takes an immediate internal measurement; stores to EEPROM    |
| `READ_CALIBRATION` | `0x06` | write+read | Already implemented; reads 17 bytes                          |
| `FACTORY_RESET`    | `0xFB` | write-only | Wipes calibration and resets I2C address to `0x20` in EEPROM |

`CALIBRATE_DRY` and `CALIBRATE_WET` do **not** reuse the last TRIGGER result — the ATtiny takes a
fresh internal measurement at the moment the command is received and writes those raw tick counts
directly to EEPROM.

All write-only transactions follow the TRIGGER pattern (`[cmd] STOP`, no subsequent read).
`READ_CALIBRATION` uses the same write-then-read pattern as `CMD_READ`.

Add the new command byte constants to `SpadefootToadSensor`:

```cpp
static constexpr uint8_t CMD_CALIBRATE_DRY = 0x04;
static constexpr uint8_t CMD_CALIBRATE_WET = 0x05;
static constexpr uint8_t CMD_FACTORY_RESET = 0xFB;
// CMD_READ_CALIBRATION = 0x06 already present
```

## MQTT commands

All three commands are registered in the factory lambda, after the sensor is constructed:

```cpp
auto sensor = std::make_shared<SpadefootToadSensor>(...);

params.registerFeature("moisture", ...);
params.registerFeature("temperature", ...);

params.registerCommand("calibrate", [sensor](const JsonObject& req, JsonObject&) {
    auto ref = req["reference"].as<CalibrationReference>();
    sensor->calibrate(ref);
});

params.registerCommand("read-calibration", [sensor](const JsonObject&, JsonObject& res) {
    sensor->readCalibration(res);
});

params.registerCommand("factory-reset", [sensor](const JsonObject&, JsonObject&) {
    sensor->factoryReset();
});
```

### `calibrate`

**Request:** `{ "reference": "dry" }` or `{ "reference": "wet" }`

**Response:** none (empty `JsonObject` → no response published)

**Behavior:**

1. Select `CMD_CALIBRATE_DRY` or `CMD_CALIBRATE_WET` based on `reference`.
2. Send a write-only I2C transaction: `[cmd] STOP`.
3. Log at INFO: `"Spadefoot Toad '%s': calibrating %s reference"`.

The ATtiny takes a fresh internal measurement immediately on receiving the command and writes the
raw tick counts to EEPROM — it does **not** reuse the last TRIGGER result.

```cpp
void calibrate(CalibrationReference ref) {
    uint8_t cmd = (ref == CalibrationReference::Wet) ? CMD_CALIBRATE_WET : CMD_CALIBRATE_DRY;
    LOGTI(ENV, "Spadefoot Toad '%s': calibrating %s reference",
        getName().c_str(), ref == CalibrationReference::Wet ? "wet" : "dry");
    device->writeByte(cmd);
}
```

### `read-calibration`

**Request:** _(empty payload)_

**Response:**

```jsonc
{
  "checksumOk": true,
  "top-front":    { "dry": 1234, "wet": 5678 },
  "top-rear":     { "dry": 1234, "wet": 5678 },
  "bottom-front": { "dry": 1234, "wet": 5678 },
  "bottom-rear":  { "dry": 1234, "wet": 5678 },
}
```

Values of `0xFFFF` indicate that the corresponding reference has not yet been calibrated.
If the checksum does not match, return `{ "checksumOk": false }` with no probe fields.

**Behavior:** issue `CMD_READ_CALIBRATION`, read 17 bytes, verify XOR checksum (same logic
already present in the constructor), populate response JSON.

### `factory-reset`

**Request:** _(empty payload)_

**Response:** none

**Behavior:** send `CMD_FACTORY_RESET` as a write-only I2C transaction. The ATtiny wipes all
calibration slots to `0xFFFF` and resets the I2C address to `0x20` in EEPROM; the new address
takes effect after the next ATtiny reset. Log at WARN:
`"Spadefoot Toad '%s': factory reset issued"`.

```cpp
void factoryReset() {
    LOGTW(ENV, "Spadefoot Toad '%s': factory reset issued", getName().c_str());
    device->writeByte(CMD_FACTORY_RESET);
}
```

## Changes to `SpadefootToadSensor.hpp`

Summary of all additions to the class:

- Three new `CMD_*` constants (`0x04`, `0x05`, `0xFB`).
- Three new public methods: `calibrate()`, `readCalibration()`, `factoryReset()`.
- `SpadefootToadSensorSettings` gains no new fields (no new configuration).
- `CalibrationReference` enum + `ArduinoJson::Converter<CalibrationReference>` added in the same file.
- Factory lambda gains three `params.registerCommand(...)` calls.

## Constructor calibration logging

The existing constructor already reads and logs calibration data. Keep this behaviour unchanged —
it provides a useful baseline snapshot in the boot log. The new `read-calibration` command is an
on-demand read for interactive use.

## Open questions

1. **Calibration timing** — `CALIBRATE_DRY`/`WET` take an internal measurement synchronously
   before returning. Does the ATtiny need any additional settling time after the EEPROM write
   before normal TRIGGER measurements can safely resume? If so, should the driver insert a delay
   or just rely on the natural measurement cadence?
2. **Access control** — MQTT command access control (admin-only) is enforced at the broker layer,
   not in the firmware. Document this expectation in the admin tooling, not here.
