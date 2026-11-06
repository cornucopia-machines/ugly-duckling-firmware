# Electric fence monitor

Watches an electric fence and reports its voltage level. Each input pin stands for a voltage: a
pulse detected on the pin means the fence reached that voltage. Every measurement period the
monitor reports the highest voltage whose pin saw a pulse, or 0 if none did.

Not in use on any deployed device today, and likely to change substantially before it is.

## Hardware support

Each pin connects to a GPIO of the microcontroller, which counts the pulses on it.

## Configuration

Add an `electric-fence` peripheral with one entry in `pins` per voltage level. For example, on MK6
(the voltages are only illustrative):

```json
{
  "peripherals": [
    {
      "name": "fence",
      "type": "electric-fence",
      "params": {
        "pins": [
          { "pin": "A1", "voltage": 1000 },
          { "pin": "A2", "voltage": 3000 }
        ]
      }
    }
  ]
}
```

| Parameter | Default | Meaning |
| --- | --- | --- |
| `pins` | `[]` | List of `{ "pin": ..., "voltage": ... }` entries. With none, the value is always `0` |
| `pins[].pin` | (required) | An internal pin name or GPIO number |
| `pins[].voltage` | (required) | The voltage, in volts, that a pulse on this pin stands for (0–65535) |
| `measurementFrequency` | `10` | Seconds per measurement period |

## Telemetry

The monitor publishes one `voltage` feature under its peripheral name:

| Field | Unit | Value |
| --- | --- | --- |
| `value` | V | Highest `voltage` among the pins that saw a pulse in the last measurement period; `0` if none did |

Until the first period ends, the value is `0`.

## Behavior

Pulses are counted in the background, also while the device is in light sleep, the same way as
for the [flow meter](FlowMeter.md#how-counting-works): falling edges count, and there is no
debouncing. At the end of each period the monitor collects and clears every pin's count.

## Limitations

- `measurementFrequency` must be longer than the time between the fence's pulses, otherwise
  periods without a pulse report `0`.
- Each pin on a coprocessor-capable GPIO uses one of the 4 pulse-counting channels shared with
  flow meters (see [Flow meter § Limitations](FlowMeter.md#limitations)).
