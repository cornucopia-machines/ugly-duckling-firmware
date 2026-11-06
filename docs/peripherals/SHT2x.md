# SHT2x / HTU2x air sensor

SHT2x and HTU2x sensors measure air temperature and relative humidity. They connect to an I²C
bus. Both use the same driver (the Si7021 one), registered under two types: `environment:sht2x`
and `environment:htu2x`. The two behave the same; the type only shows up in the log.

## Hardware support

Any Ugly Duckling with an I²C bus can read it. The repository has templates for an external
sensor on MK5 (`SDA` / `SCL`), MK7 (`A2` / `A4`) and MK8 (`A2` / `A1`).

## Configuration

```json
{
  "peripherals": [
    {
      "name": "external-environment",
      "type": "environment:htu2x",
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
| `sda`, `scl` | (required) | I²C pins the sensor is on, e.g. `SDA`, `SCL` on MK5 |

The address is fixed at 0x40, and the driver sets its own I²C clock: `address` and `clkSpeed` are
ignored.

Templates:
[`htu21-external-environment-mk5.json`](../../config-templates/htu21-external-environment-mk5.json),
[`i2c-mk7.json`](../../config-templates/i2c-mk7.json),
[`environment-mk8.json`](../../config-templates/environment-mk8.json).

## Telemetry

| Feature | Unit | Value |
| --- | --- | --- |
| `temperature` | °C | Air temperature |
| `moisture` | % | Relative humidity |

If a reading fails, the value is `null`.

## Behavior

Each feature is measured separately whenever telemetry is collected; there is no caching, so
temperature and humidity come from two measurements.

The firmware doesn't talk to the sensor at startup, so a missing sensor doesn't stop the
peripheral from starting; it only reports `null` values.

## Limitations

- Fixed address 0x40, so only one sensor per I²C bus.
- No reset, heater or resolution settings.
