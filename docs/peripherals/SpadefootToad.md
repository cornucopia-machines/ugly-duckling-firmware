# Spadefoot Toad soil sensor

Spadefoot Toad is Cornucopia's own capacitive soil probe. It measures soil moisture and temperature
at two depths, about 5 cm and 15 cm, with an electrode pair on the front and rear face of the
probe at each depth and an NTC thermistor at each depth. An ATtiny1616 on the probe does the
measuring and answers over I²C.

- Hardware: [spadefoot-toad-hardware](https://github.com/cornucopia-machines/spadefoot-toad-hardware)
- Firmware, and the `probe.py` tool for working with the probe from a computer:
  [spadefoot-toad-firmware](https://github.com/cornucopia-machines/spadefoot-toad-firmware)

## Which type to use

| Type | Transport | Use on |
| --- | --- | --- |
| `soil:spadefoot-toad` | Hardware I²C | Every Ugly Duckling with an I²C port, except MK10 |
| `soil:spadefoot-toad-bb` | Software (bit-banged) I²C | MK10 only |

On MK10 the external I²C connector (`EXT_SDA`, `EXT_SCL`) can't reach a free hardware I²C
controller: the ESP32-C6 has one routable controller, and MK10 uses it for the internal bus. The
firmware drives the probe in software there instead, and registers `soil:spadefoot-toad-bb` only
on MK10. From MK11 on, the internal bus moved to the C6's other controller, so the external port
has hardware I²C and the regular type works.

So on MK10, Spadefoot Toad is the only peripheral that works on the external I²C port.

On MK14 the I²C port isn't powered for sensor readings yet
([#713](https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/713)).

The two types behave the same apart from the transport.

## Wiring

The probe has a 4-pin JST PH (2.0 mm) connector, `JI2C1`, like Ugly Duckling's I²C port.
Connect the two with a direct PH2.0 4-pin cable, the kind used with the probes in service today.

The probe has no I²C pull-up resistors of its own. Ugly Duckling's I²C port has them on the board
(2.2 kΩ on MK13), so nothing needs adding; on a bench adapter without pull-ups, the probe's
documentation asks for 4.7 kΩ to 3.3 V.

The bus runs at 100 kHz: the probe sleeps between transactions and wakes on its own address, which
does not work at 400 kHz.

## Configuration

```json
{
  "peripherals": [
    {
      "name": "soil",
      "type": "soil:spadefoot-toad",
      "params": {
        "sda": "EXT_SDA",
        "scl": "EXT_SCL"
      }
    }
  ]
}
```

On MK10, use `"type": "soil:spadefoot-toad-bb"` with the same parameters.

| Parameter | Default | Meaning |
| --- | --- | --- |
| `sda`, `scl` | (required) | I²C pins the probe is on, e.g. `EXT_SDA`, `EXT_SCL` |
| `address` | `0x20` | The probe's I²C address (a string, e.g. `"0x21"`) |
| `logRawValues` | `false` | Also read and log the raw electrode tick counts and thermistor ADC values on every reading, for debugging |

The clock is always 100 kHz; `clkSpeed` is ignored.

The probe stores its address in its own EEPROM. Ugly Duckling doesn't change it; use `probe.py
--set-address` from the firmware repository to give a probe a different address, for example to
put two on one bus. Note that the [Chirp](Chirp.md) sensor also defaults to `0x20`.

Templates:
[`plot-controller-mk9-spadefoot-toad.json`](../../config-templates/plot-controller-mk9-spadefoot-toad.json),
[`plot-controller-mk10-spadefoot-toad.json`](../../config-templates/plot-controller-mk10-spadefoot-toad.json)
(bit-banged),
[`plot-controller-mk11-spadefoot-toad.json`](../../config-templates/plot-controller-mk11-spadefoot-toad.json),
[`plot-controller-mk12-spadefoot-toad.json`](../../config-templates/plot-controller-mk12-spadefoot-toad.json).

## Telemetry

| Feature | Unit | Value |
| --- | --- | --- |
| `moisture` | % | Mean of the valid electrode pairs' readings |
| `temperature` | °C | Mean of the top and bottom thermistors |

The probe computes each electrode pair's percentage from its own calibration: 0 % is the reading
stored as the dry reference (dry air), 100 % the reading stored as the wet reference (submerged
in water), clamped to 0–100. This is not volumetric water content.

An electrode pair the probe marks invalid (not calibrated, or timed out) is left out of the mean.
If none is valid, `moisture` is `null`. `temperature` is `null` unless both thermistors read
valid.

## Calibration

The probe needs a dry and a wet reference for each electrode pair before it reports moisture. A
fresh or factory-reset probe has none, and reports every pair as invalid.

The peripheral accepts these commands on `.../peripherals/<name>/commands/<command>` (see
[Remote commands](../../README.md#remote-commands)), and answers on
`.../peripherals/<name>/responses/<command>`:

| Command | Payload | Effect |
| --- | --- | --- |
| `calibrate` | `{"reference": "dry"}` or `{"reference": "wet"}` | The probe measures immediately and stores the result as the dry (probe in dry air) or wet (probe fully submerged in water) reference. A missing or unknown `reference` means `dry` |
| `read-calibration` | `{}` | Returns the stored dry and wet references for `top-front`, `top-rear`, `bottom-front` and `bottom-rear`, and `checksumOk` |
| `factory-reset` | `{}` | Wipes the probe's calibration and resets its I²C address to `0x20` (effective after the probe restarts) |

A failed command returns its reason in `error`.

## How a reading works

1. Send the probe a trigger. It wakes, measures all four electrode pairs and both thermistors,
   and goes back to sleep.
2. Wait 1 s, then read the 10-byte result and check its checksum.

A reading takes about a second, and serves both features: a new one is taken at most every 2 s.

If a reading fails (no answer, NACK, bad checksum), the device logs a warning and keeps reporting
the last good values; before the first good reading these are `null`. The next request tries
again.

At startup the firmware checks the probe's manufacturer and device IDs, then logs its firmware and
hardware revisions and the stored calibration. A missing probe, or a different device at the
address, fails the peripheral.

## Troubleshooting

| Symptom in the log | Likely cause |
| --- | --- |
| `unexpected manufacturer ID` or `unexpected device ID` at startup | Another I²C device at the address, e.g. a Chirp sensor (also `0x20`) |
| `NACK on address (write)` (bit-banged), or I²C errors at startup | Wiring, missing pull-ups, or `address` doesn't match the probe |
| `BitbangI2C: clock stretch timeout` | SCL is held low: check the wiring |
| `moisture TF=255 TR=255 BF=255 BR=255` and `moisture` is `null` | Probe not calibrated; run `calibrate` with `dry`, then `wet` |
| `calibration checksum mismatch` at startup | The calibration read was garbled or the stored calibration is corrupt; check with `read-calibration` |
| `checksum mismatch` during readings | Noise on the bus; the reading is discarded |
| `temperature valid flag set but individual reading(s) invalid` | One of the thermistors gives no valid reading |

## Limitations

- One `moisture` value per probe: the two depths and two faces are averaged, not reported
  separately.
- The probe's I²C address can't be changed from Ugly Duckling.
- `soil:spadefoot-toad-bb` exists on MK10 only, and only for this probe.
