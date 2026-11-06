# Sensirion SHT3x air sensor

The SHT3x measures air temperature and relative humidity. It connects to an I²C bus.

## Hardware support

MK9 and MK10 have an SHT3x soldered on the board, on the internal `SDA` / `SCL` bus. Any Ugly
Duckling with an I²C port can also read one on an external I²C dongle.

## Configuration

```json
{
  "peripherals": [
    {
      "name": "air",
      "type": "environment:sht3x",
      "params": {
        "sda": "SDA",
        "scl": "SCL"
      }
    }
  ]
}
```

| Parameter | Default | Meaning |
| --- | --- | --- |
| `sda`, `scl` | (required) | I²C pins the sensor is on, e.g. `SDA`, `SCL` on MK9 and MK10 |
| `address` | `0x44` | The sensor's I²C address (a string). The SHT3x can also be at `"0x45"` |
| `clkSpeed` | `400` | I²C clock in kHz (the SHT3x supports up to 1000) |

Templates: [`plot-controller-mk9.json`](../../config-templates/plot-controller-mk9.json),
[`plot-controller-mk10.json`](../../config-templates/plot-controller-mk10.json).

## Telemetry

| Feature | Unit | Value |
| --- | --- | --- |
| `temperature` | °C | Air temperature |
| `moisture` | % | Relative humidity |

If a reading fails, both values are `null`.

## Behavior

The sensor is read when telemetry is collected: one single-shot, high-repeatability measurement
gives both temperature and humidity. Readings are reused for 1 second, so the two features come
from the same measurement. A failed reading also counts, so the sensor is tried again after a
second.

At startup the firmware initializes the sensor; if it doesn't answer, the peripheral fails to
start.

## Limitations

- The measurement mode is fixed: no periodic mode, no heater, no soft or hard reset command.
