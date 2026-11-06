# Door

A `door` moves a motorized door, such as a chicken coop door, between two end switches: one that
signals the door is fully open, and one that signals it is fully closed. A `chicken-door`
function decides when the door should be open; the door peripheral runs the motor until the
matching end switch engages, and stops it if that takes too long.

## Hardware support

Every Ugly Duckling from MK5 to MK14 supports doors. The door's motor runs from one of the board's
motor (H-bridge) outputs:

| Ugly Duckling | Motor outputs |
| --- | --- |
| MK5 to MK13 | `a`, `b` |
| MK14 | `a` only (connector JDRIVE1) |

The end switches must be on the board's own GPIO pins; pins on an I/O expander can't be used.

## Wiring

- **Motor:** connect it to the motor output named in `motor`. The board drives the output
  "forward" to open the door and "reverse" to close it at full power. If the door moves the wrong
  way, swap the motor's two wires.
- **End switches:** by default each switch connects its pin to GND when engaged; the firmware
  enables the pin's internal pull-up. With `invertSwitches` set to `true`, the firmware enables
  the internal pull-down instead, and a switch counts as engaged when it pulls its pin high.

Switch changes are debounced for 50 ms.

## Configuration

A door on motor output `b` of an MK6, with its end switches on pins `A2` (open) and `A1`
(closed), controlled by a `chicken-door` function:

```json
{
  "peripherals": [
    {
      "name": "door",
      "type": "door",
      "params": {
        "motor": "b",
        "openPin": "A2",
        "closedPin": "A1"
      }
    }
  ],
  "functions": [
    {
      "name": "chicken-door",
      "type": "chicken-door",
      "params": {
        "door": "door",
        "lightSensor": "light-sensor"
      }
    }
  ]
}
```

| Parameter | Default | Meaning |
| --- | --- | --- |
| `motor` | (required) | The motor output that drives the door: `a` or `b`. Can be left out on a board with a single output (MK14) |
| `openPin` | (required) | The pin of the switch that engages when the door is fully open |
| `closedPin` | (required) | The pin of the switch that engages when the door is fully closed |
| `invertSwitches` | `false` | `false`: switches pull their pin to GND when engaged. `true`: switches pull their pin high when engaged |
| `movementTimeout` | `60` | **Seconds** (not milliseconds) the motor may run without reaching the target switch. After that the door stops until restart, see below |

Templates: [`chicken-door-mk6.json`](../../config-templates/chicken-door-mk6.json) and
[`chicken-door-wokwi.json`](../../config-templates/chicken-door-wokwi.json).

## Telemetry

Each door publishes one `door` feature under its peripheral name, for example:

```json
{
  "state": 1,
  "targetState": "Open",
  "operationState": 0
}
```

| Field | Value |
| --- | --- |
| `state` | Where the door is: `1` open, `-1` closed. Missing while the position is unknown |
| `targetState` | Where the door should be: `Open` or `Closed`. Missing until the function sets it |
| `operationState` | `0` running normally, `1` stopped because the device shut down, `2` stopped because `movementTimeout` ran out |

The door publishes telemetry right away when it starts or stops moving, or its position changes.

## Controlling the door

The door accepts no commands of its own. It is moved by the `chicken-door` function it is assigned
to (the function's `door` parameter). Schedules and manual overrides (`overrideState`,
`overrideUntil`) are set in that function's configuration. When nothing in the function's
configuration calls for the door to be open, the function closes it.

## How the door moves

1. At startup the motor is stopped, and the door waits for the function to set a target.
2. When the target is open and the open switch isn't engaged, the motor runs forward; when the
   target is closed and the closed switch isn't engaged, it runs in reverse.
3. When the matching switch engages, the motor stops.

The position comes from the switches:

- Only the open switch engaged: open. Only the closed switch engaged: closed.
- Both engaged: unknown, and the device logs a warning asking whether the switches should be
  inverted.
- Neither engaged: unknown, unless the door had already reached its target. Then the firmware
  assumes it is still there, so a switch that stops making contact doesn't make the door move.

## Safety behavior

- **Movement timeout:** while the motor runs, a `movementTimeout` countdown runs too. It starts
  again whenever a switch changes or the target changes. If the door hasn't reached its target
  switch when it runs out, the motor stops, the device logs
  `Watchdog timed out, stopping operation`, and `operationState` becomes `2`. The door then
  ignores every further request until the device restarts.
- **Shutdown:** when the device shuts down because its battery is low, it stops the motor and
  `operationState` becomes `1`.

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `Both open and closed switches are engaged, should the switches be inverted?` in the log | The switches don't work the way `invertSwitches` expects |
| `operationState` is `2` | The door didn't reach its switch within `movementTimeout`: it is stuck, the switch is misplaced or not wired, or the motor runs the wrong way. Fix the cause, then restart the device |
| `failed to find motor` | `motor` is missing or names an output the board doesn't have |

## Limitations

- After a movement timeout the door stays stopped until the device restarts.
- End switches must be on the board's own GPIO pins.
- The motor always runs at full power.
