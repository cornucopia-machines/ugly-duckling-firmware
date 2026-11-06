# NTC thermistor

An NTC thermistor in a voltage divider, read with the ESP32's ADC, measures temperature.

## Wiring

The conversion assumes this divider:

- the thermistor between the ADC pin and GND,
- a fixed resistor between the ADC pin and the supply, with the same resistance as the thermistor
  at 25 °C (e.g. 10 kΩ for a 10 kΩ thermistor).

## Configuration

```json
{
  "peripherals": [
    {
      "name": "soil-temperature",
      "type": "environment:ntc-temperature-sensor",
      "params": {
        "pin": "A2"
      }
    }
  ]
}
```

| Parameter | Default | Meaning |
| --- | --- | --- |
| `pin` | (required) | ADC-capable pin at the divider's midpoint |
| `beta` | `3950` | The thermistor's β coefficient, from its datasheet |

Template: [`plot-controller-wokwi.json`](../../config-templates/plot-controller-wokwi.json), which
feeds it into a [Kalman filter](KalmanSoilMoisture.md).

## Telemetry

| Feature | Unit | Value |
| --- | --- | --- |
| `temperature` | °C | From the β equation, with 25 °C as the reference temperature |

## Behavior

A new reading is taken at most once a second. If the ADC read fails, the last value is kept
(`null` before the first good reading).

A pin that can't do ADC fails the peripheral at startup with `Pin ... cannot be used for analog
reads`.

## Limitations

- The conversion takes the ADC's full-scale reading (4095) to be the divider's supply voltage,
  and there is no calibration offset to correct for a divider that doesn't match.
- Only the divider above is supported: no other fixed resistor value, and no thermistor on the
  supply side.
