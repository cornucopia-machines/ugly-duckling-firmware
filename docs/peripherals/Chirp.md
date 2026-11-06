# Chirp soil sensor

The [Chirp!](https://wemakethings.net/chirp/) is a capacitive soil moisture sensor with an I²C
interface. Ugly Duckling reads its capacitance, converted to a moisture percentage with your
calibration, and its temperature. Its light sensor is not used.

## Configuration

Connect the sensor to an I²C port, e.g. the external one on MK9:

```json
{
  "peripherals": [
    {
      "name": "soil",
      "type": "soil:chirp",
      "params": {
        "sda": "EXT_SDA",
        "scl": "EXT_SCL",
        "air": 250,
        "water": 550
      }
    }
  ]
}
```

| Parameter | Default | Meaning |
| --- | --- | --- |
| `sda`, `scl` | (required) | I²C pins the sensor is on |
| `address` | `0x20` | The sensor's I²C address (a string, e.g. `"0x21"`) |
| `clkSpeed` | `400` | I²C clock in kHz |
| `air` | `250` | Raw capacitance reading in dry air (0 %) |
| `water` | `550` | Raw capacitance reading in water (100 %) |

Template: [`plot-controller-mk9-chirp.json`](../../config-templates/plot-controller-mk9-chirp.json).

### Calibration

Every sensor reads a little differently, so measure `air` and `water` for each one: hold it in
dry air, then in water, and note the raw capacitance. The raw value is logged at verbose level
(`Chirp capacitance: ...`).

## Telemetry

| Feature | Unit | Value |
| --- | --- | --- |
| `moisture` | % | `(raw − air) × 100 / (water − air)` |
| `temperature` | °C | The sensor's temperature reading (reported in 0.1 °C steps) |

The moisture value is not clamped, so it can go below 0 % or above 100 % when the soil is drier
or wetter than the calibration points.

## Behavior

A reading reads the capacitance and the temperature registers; it is repeated at most once a
second. If one of the two reads fails, the device logs a warning (`Failed to read Chirp
capacitance` or `Failed to read Chirp temperature`) and that value is `null` until the next
successful reading.

The sensor isn't contacted at startup, so a missing sensor shows up only as these warnings.

## Limitations

- The light sensor is not supported.
- The Chirp and the [Spadefoot Toad](SpadefootToad.md) both default to address `0x20`.
