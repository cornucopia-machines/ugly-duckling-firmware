# Analog meter

A generic sensor with an analog voltage output, such as a pressure sensor. The firmware samples
the voltage with the microcontroller's ADC, scales the reading linearly, and reports a moving
average.

## Hardware support

The sensor's output connects to a GPIO of the microcontroller that can do analog reads. A pin
without an ADC channel fails the peripheral with `Pin ... cannot be used for analog reads`. Pins
of an I/O expander such as the [XL9535](XL9535.md) can't be used.

## Configuration

Add an `analog-meter` peripheral. For example, a pressure sensor on MK6:

```json
{
  "peripherals": [
    {
      "name": "pressure",
      "type": "analog-meter",
      "params": {
        "pin": "A2",
        "type": "pressure"
      }
    }
  ]
}
```

| Parameter | Default | Meaning |
| --- | --- | --- |
| `pin` | (required) | The pin the sensor's output is on, an internal pin name or a GPIO number |
| `type` | `analog-meter` | The feature type the value is published as, e.g. `pressure` |
| `offset` | `0.0` | Added to the scaled reading |
| `multiplier` | `1.0` | The raw reading is multiplied by this |
| `measurementFrequency` | `1000` | Milliseconds between readings |
| `windowSize` | `1` | Number of readings the reported value averages; `1` reports the latest |

The reported value is `offset + raw × multiplier`, where `raw` is the uncalibrated 12-bit ADC
reading (0–4095), taken with 12 dB attenuation. Choose `offset` and `multiplier` to convert it to
the unit the sensor measures.

Template: [`pressure-mk6.json`](../../config-templates/pressure-mk6.json).

## Telemetry

The meter publishes one feature under its peripheral name, of the type set by `type`:

| Field | Unit | Value |
| --- | --- | --- |
| `value` | As scaled by `offset` and `multiplier` | Average of the last `windowSize` readings |

A failed read is skipped and the average keeps the earlier readings. Before the first successful
reading the value is `0`.

## Limitations

- Only linear scaling, applied to the raw reading: the ADC's eFuse calibration is not used, so
  the raw-to-voltage relationship can differ between chips.
- The value is never `null`, and a disconnected sensor is not detected: the meter reports
  whatever the pin reads.
