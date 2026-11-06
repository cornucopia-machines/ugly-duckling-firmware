# Analog light sensor

Estimates ambient light in lux from a photoresistor (LDR) read on an analog pin. Like every light
sensor, it can drive the chicken door function.

It exists for the Wokwi simulation only; it is not meant for real hardware.

## Hardware support

The sensor connects to an ADC pin. The repository uses it in the chicken door Wokwi simulation
([`diagram.chicken-door.json`](../../wokwi/diagram.chicken-door.json)), on pin `C4`.

## Configuration

```json
{
  "peripherals": [
    {
      "name": "light-sensor",
      "type": "light-sensor:analog",
      "params": {
        "pin": "C4"
      }
    }
  ]
}
```

| Parameter | Default | Meaning |
| --- | --- | --- |
| `pin` | (required) | The analog pin the sensor is on |
| `gamma` | `0.7` | The photoresistor's gamma (slope of its log resistance vs. log lux curve) |
| `rl10` | `50.0` | The photoresistor's resistance at 10 lx, in kΩ |
| `measurementFrequency` | `1` | Seconds between readings |
| `latencyInterval` | `5` | Seconds of readings to average over |

Template: [`chicken-door-wokwi.json`](../../config-templates/chicken-door-wokwi.json).

## Telemetry

| Feature | Unit | Value |
| --- | --- | --- |
| `light` | lx | Ambient light, averaged |

## Behavior

A background task reads the pin every `measurementFrequency` seconds and converts it to lux. The
conversion assumes a 5 V reference and a 2 kΩ resistor in the divider with the photoresistor:

```text
voltage    = reading / 4096 × 5 V
resistance = 2000 Ω × voltage / (1 − voltage / 5 V)
lux        = (rl10 × 1000 × 10^gamma / resistance)^(1 / gamma)
```

The reported value is the average of the last `latencyInterval` / `measurementFrequency` readings
(5 by default). A failed reading is skipped, so the average keeps its previous value. Before the
first reading the value is `0`.

## Limitations

- Simulation only. The 5 V reference and 2 kΩ divider are fixed to match the Wokwi part.
- `latencyInterval` must be at least `measurementFrequency`.
