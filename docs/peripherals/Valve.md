# Valve

A `valve` opens and closes a water valve. Usually it is a solenoid valve driven by one of the
board's motor (H-bridge) outputs, but it can also be switched by a single pin, for example a relay
on an I/O expander. A [plot controller](../Components.md) function decides when the valve is
open; the valve itself only carries out the transitions and reports its state.

## Hardware support

Every Ugly Duckling from MK5 to MK14 supports valves. The motor outputs differ by board:

| Ugly Duckling | Motor driver | Motor outputs |
| --- | --- | --- |
| MK5 | Two DRV8874 | `a`, `b` |
| MK6, MK7 | DRV8833 | `a`, `b` |
| MK8 to MK13 | DRV8848 | `a`, `b` |
| MK14 | DRV8847 | `a` only (connector JDRIVE1) |

On every board the default `strategy` is `latching`.

## Valve types

The `strategy` parameter says how the valve is driven:

| Strategy | Valve | Open | Closed |
| --- | --- | --- | --- |
| `latching` | Latching (bistable) solenoid | A short pulse in one direction | A short pulse in the other direction |
| `NC` | Normally closed | Driven, and kept driven | Not driven |
| `NO` | Normally open | Not driven | Driven, and kept driven |

A latching valve stays where it was last switched without power, so the board only draws current
during the pulse. `NC` and `NO` valves are powered for as long as they are in their non-default
state.

When `pin` is set instead of a motor, the pin is driven high to open the valve and low to close
it, and `strategy` is ignored.

## Wiring

Connect the valve's two wires to the motor output named in `motor`. The board drives the output
"forward" to open the valve and "reverse" to close it. If a latching valve opens when it should
close, swap its two wires.

On MK14 the only output is JDRIVE1. MK13 and earlier had a second output on bridge B.

## Configuration

A latching valve on motor output `a`:

```json
{
  "peripherals": [
    {
      "name": "valve",
      "type": "valve",
      "params": {
        "motor": "a"
      }
    }
  ]
}
```

A valve switched by output 0 of an I/O expander peripheral named `relay`:

```json
{
  "peripherals": [
    {
      "name": "valve-1",
      "type": "valve",
      "params": {
        "pin": "relay:0"
      }
    }
  ]
}
```

| Parameter | Default | Meaning |
| --- | --- | --- |
| `motor` | (required unless `pin` is set) | The motor output that drives the valve: `a` or `b`. Can be left out on a board with a single output (MK14) |
| `pin` | (none) | A pin to switch the valve with instead of a motor output. An on-board pin or an I/O expander pin such as `relay:0`. When set, all other parameters are ignored |
| `strategy` | `latching` | How the valve is driven: `latching`, `NC` or `NO` |
| `switchDuration` | `30` for `latching`, `500` for `NC` and `NO` | Milliseconds to drive the valve at full power when switching it |
| `holdDuty` | `100` | `NC` and `NO`: power, in percent, used to keep the valve in its driven state after `switchDuration`. Latching: power of the switching pulse |
| `brakeDuration` | `10` | `latching` only: milliseconds to brake the output after the pulse before releasing it |

A latching solenoid only needs its rated pulse (25 to 30 ms for the S211B and Transol coils). A
longer pulse only heats the coil and loads the supply, so don't copy a `switchDuration` meant for
a motorized valve to a latching one. Ending the pulse with a brake lets the coil's current die
down inside the driver instead of flowing back onto the supply; `brakeDuration` should cover a
few time constants of the coil, which is 2 to 3 ms for the valves in use.

The firmware reads `holdDuty` for latching valves too: the pulse runs at `holdDuty` percent power.
Leave it at the default `100` for latching valves.

An unknown `strategy` value is logged as an error and treated as `NC`.

Templates: the `plot-controller-mk*.json` files in
[`config-templates/`](../../config-templates/) (one per board), and
[`multiplexer-mk6.json`](../../config-templates/multiplexer-mk6.json) for a valve on an I/O
expander pin.

## Telemetry

Each valve publishes one `valve` feature under its peripheral name:

```json
{
  "state": "Open"
}
```

`state` is `Open` or `Closed`: the state the firmware last switched the valve to. The firmware
does not sense the valve's actual position.

## Controlling the valve

The valve accepts no commands of its own. It is opened and closed by the `plot-controller`
function it is assigned to (the function's `valve` parameter). Schedules and manual overrides
(`overrideState`, `overrideUntil`) are set in that function's configuration. When nothing in the
function's configuration calls for the valve to be open, the function closes it.

## Behavior worth knowing

- **At startup** the firmware puts the valve in its default state without waiting for the
  function: a latching valve gets a closing pulse, an `NC` valve is left undriven (closed), an
  `NO` valve is left undriven (open), and a `pin` valve has its pin driven low (closed).
- **Switching to the state the valve is already in does nothing:** a latching valve is not pulsed
  again.
- **When the device shuts down** because its battery is low, it closes every valve first.

## Limitations

- The reported state is what the firmware last commanded; a valve that failed to switch, or was
  switched by hand, is not detected.
- MK14 has a single motor output, so it can drive only one motor-driven valve.
- `NC` and `NO` valves draw current for as long as they are held in their driven state.
