# Flow meter

A pulse-output flow meter on the water line, such as a Hall-effect turbine sensor. The firmware
counts the meter's pulses, converts them to liters, and reports the volume that flowed and the
average flow rate. The [plot controller](../Components.md) uses it to measure how much water a
valve has let through.

## Hardware support

The meter's pulse output connects to one of Ugly Duckling's GPIOs. MK9 and later have dedicated
flow meter inputs:

| Ugly Duckling | Flow meter pins |
| --- | --- |
| MK5–MK8 | No `IFLOW` pins; the templates use port pin `B1` |
| MK9–MK13 | `IFLOWA`, `IFLOWB` |
| MK14 | `IFLOWA` only (MK13 and earlier had a second one) |

Any other internal pin name, or a raw GPIO number, works too.

## Configuration

Add a `flow-meter` peripheral. For example, on MK12:

```json
{
  "peripherals": [
    {
      "name": "flow-meter",
      "type": "flow-meter",
      "params": {
        "pin": "IFLOWA",
        "qFactor": 7.463
      }
    }
  ]
}
```

| Parameter | Default | Meaning |
| --- | --- | --- |
| `pin` | (required) | The pin the meter's pulse output is on, e.g. `IFLOWA` |
| `qFactor` | `7.5` | Pulses per second at a flow of 1 l/min, from the meter's datasheet. `7.5` is the YF-S201's; use `7.463` for the flow meter in the MFZ2727 valve that ships with Cornucopia grow machines |
| `measurementFrequency` | `1000` | Milliseconds between collecting the counted pulses |

The volume of one pulse is `1 / (qFactor × 60)` liters, so with `qFactor` 7.5 a liter is 450
pulses. To calibrate a meter, let a known volume through and scale `qFactor` by the ratio of the
reported volume to the real one.

Templates: the `plot-controller-mk*.json` files in
[`config-templates/`](../../config-templates/) pair a flow meter with a valve.

## Telemetry

The meter publishes one `flow` feature under its peripheral name:

| Field | Unit | Value |
| --- | --- | --- |
| `volume` | l | Water that flowed since the previous telemetry message |
| `rate` | l/min | Average flow rate since the previous telemetry message |

`rate` is left out if no measurement was taken since the previous message.

## How counting works

Each falling edge on the pin is one pulse, and edges within 1 ms of the previous one are ignored
as bounce. Pulses are counted in the background, also while the device is in light sleep, so no
water goes unmeasured while the device saves power:

- On pins the low-power coprocessor can watch (GPIO 0–7 on ESP32-C6, GPIO 0–21 on ESP32-S3), the
  coprocessor counts the pulses, and the main CPU doesn't wake up for them.
- On other pins, a GPIO interrupt counts the pulses; in light sleep the device wakes up on each
  level change of the pin.

Every `measurementFrequency` the firmware collects the pulse count and adds its volume to the
total. Telemetry reports, and resets, that total.

## Limitations

- The coprocessor has 4 channels, shared by every pulse-counting peripheral (flow meters and
  [electric fence](ElectricFence.md) inputs) on coprocessor-capable pins. A fifth such pin fails
  with `ULP channel limit exceeded`.
