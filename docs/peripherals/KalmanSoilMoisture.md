# Kalman-filtered soil moisture

`environment:kalman-soil-moisture` is a virtual peripheral: it has no hardware of its own. It
takes the readings of a soil moisture sensor and a soil temperature sensor, and estimates the
temperature-independent soil moisture with a Kalman filter.

Some moisture sensors read differently as the soil warms and cools, even when the water content
doesn't change. The filter models a reading as:

```text
observed moisture = real moisture + β × (temperature − tempRef)
```

and estimates both the real moisture and β, the sensor's sensitivity to temperature (in % per
°C), as readings come in.

## Configuration

Configure the two sensors as usual, and list them before the filter: peripherals are created in
the order they are listed, and the filter looks its sensors up by name. Then point the plot
controller at the filter instead of the raw sensor:

```json
{
  "peripherals": [
    {
      "name": "soil-temperature",
      "type": "environment:ds18b20",
      "params": {
        "pin": "A2"
      }
    },
    {
      "name": "raw-soil-moisture",
      "type": "soil:hw390",
      "params": {
        "pin": "A1",
        "air": 3017,
        "water": 1105
      }
    },
    {
      "name": "soil-moisture",
      "type": "environment:kalman-soil-moisture",
      "params": {
        "rawMoistureSensor": "raw-soil-moisture",
        "temperatureSensor": "soil-temperature"
      }
    }
  ]
}
```

Any peripheral with a moisture reading can be the raw sensor, and any with a temperature reading
the temperature sensor.

| Parameter | Default | Meaning |
| --- | --- | --- |
| `rawMoistureSensor` | (required) | Name of the moisture sensor peripheral |
| `temperatureSensor` | (required) | Name of the temperature sensor peripheral |
| `initialMoisture` | `50.0` | Starting estimate of the real moisture, in % |
| `initialBeta` | `0.0` | Starting estimate of β, in % per °C |
| `tempRef` | `20.0` | Reference temperature in °C: the reported moisture is what the sensor would read at this temperature |
| `qMoist` | `1e-5` | Process noise of the real moisture: higher follows real changes faster, but also more of the noise |
| `qBeta` | `1e-6` | Process noise of β; usually tiny, as β barely changes |
| `rSensitive` | `1e-3` | Measurement noise during the start-up period: low, so the estimate converges quickly from its starting values |
| `rNormal` | `1e-1` | Measurement noise after the start-up period: higher smooths more, but reacts more slowly |
| `sensitivePeriod` | `900` | Length of the start-up period, in seconds, counted from boot |

Templates: the `*-fully-equipped.json` templates for MK6–MK8
([`plot-controller-mk8-fully-equipped.json`](../../config-templates/plot-controller-mk8-fully-equipped.json)
for example) and [`plot-controller-wokwi.json`](../../config-templates/plot-controller-wokwi.json).

To try parameters on recorded data, [`tools/kalman`](../../tools/kalman/README.md) runs the same
filter over a CSV file.

## Telemetry

| Feature | Unit | Value |
| --- | --- | --- |
| `moisture` | % | Estimated real moisture at `tempRef` |
| `kalman-beta` | % per °C | Current estimate of β |

The raw sensors keep publishing their own features as well.

## Behavior

The filter is updated every time its moisture is read, by telemetry or by the plot controller,
with the sensors' current readings. The sensors limit how often they take a new reading (once a
second for HW-390, DS18B20 and NTC), so reads in quick succession feed the filter the same
readings again.

If either sensor's reading is missing, the device logs `Raw moisture reading is NaN` or
`Temperature reading is NaN`, reports `null` for `moisture`, and leaves the filter unchanged.

The filter's state is kept in memory only: after a restart it starts again from
`initialMoisture` and `initialBeta`, with a new start-up period.
