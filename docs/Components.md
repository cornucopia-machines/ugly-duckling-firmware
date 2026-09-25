# Components

```mermaid
graph TD
  Device["Device"]

  PerfValve["Valve<br/>(peripheral)"]
  PerfFlowSensor["Flow Sensor<br/>(peripheral)"]
  PerfSoilMoistureSensor["Soil Moisture Sensor<br/>(peripheral)"]
  PerfLightSensor["Light sensor<br/>(peripheral)"]

  FuncPlotController["Plot Controller<br/>(function)"]
  FuncChickenDoor["Chicken Door<br/>(function)"]

  FeatValve["Valve<br/>(feature)"]
  FeatFlow["Flow<br/>(feature)"]
  FeatSoilMoisture["Soil moisture<br/>(feature)"]
  FeatLight["Light level<br/>(feature)"]

  Device --> FuncPlotController
  Device --> FuncChickenDoor

  FuncPlotController --> PerfValve
  FuncPlotController --> PerfFlowSensor
  FuncPlotController --> PerfSoilMoistureSensor

  PerfValve --> FeatValve
  PerfFlowSensor --> FeatFlow
  PerfSoilMoistureSensor --> FeatSoilMoisture

  FuncChickenDoor --> PerfLightSensor
  PerfLightSensor --> FeatLight
```

* A **device** is an ugly duckling controller, equipped with the PCB, that you can move around the garden independently.

  A device can connect to the servers via MQTT over the wireless network.

  An example of a _device_ is an Ugly Duckling PCB, placed in the field, connected to whatever externals.

  The device publishes telemetry, and receives configuration and commands under its **device
  topic** — `d/$ID` for re-addressed devices, or `/devices/ugly-duckling/$INSTANCE` for legacy
  devices (see [`specs/device-readdressing.md`](specs/device-readdressing.md)). We'll refer to
  the device topic as `...` further down.

* A **peripheral** is an external physical element that is connected to the _device._

  An example of a _peripheral_ is a flow-meter sensor, a thermometer, a valve or a motor.

  Peripherals have names that identify them in the context of their owning device. They also have types that define their functionality,
  and the features they expose.

  Peripherals are initialized based on parameters in the **device configuration**, a reconciled
  configuration the server pushes via `UPDATE` (see [`Configuration.md`](Configuration.md)); `config-templates/*device-config*.json` are schema/fixture examples, not a runtime source.

* Peripherals publish telemetry as **features**

  An SHT3x peripheral for example will publish two numeric features: the `temperature` of the air and the `moisture` percentage,
  A `valve` will publish a sigle `valve` feature that includes the state of the valve and any overrides being active at the time.

  Features are addressed by their type and the name of the peripheral they belong to.

  Telemetry for each features is published as part of the device's telemetry message under `.../telemetry`.
  The features are listed under `features` in the telemetry message with `type` and `name` specified.
  We publish telemetry as a single message per device to reduce the number of topics published.
  Doing so also ensures atomic updates to telemetry.

* Peripherals are grouped into **functions**

  A _function_ is a logical grouping of specific peripherals that constitute a real-world function of the device.

  For example, a device can have a "plot controller" function.
  This requires a valve peripheral to be present, and can also have a flow sensor be attached.
  Furthermore, the plot controller function can utilize a soil moisture and a soil temperature sensor peripheral to improve irrigation accuracy.

  Another example of a function is the "chicken door": it requires a motor peripheral and a light sensor peripheral to be present.
  It can also utilize limit switch peripherals to sense when the door is fully opened or closed.

  Functions are the unit of runtime configuration.
  A plot controller can be configured to open the valve on a schedule, or to ensure the soil moisture level stays between 60-80%.
  Configuration is sent to the device under `.../config`, with each feature having its own configuration object.

## Valve timing

Motor-driven valves (`strategy` of `NO`, `NC` or `latching`) are driven through an H-bridge.
Their timing is set by these properties:

| Property         | Strategies | Default                                | Meaning                                                                  |
|------------------|------------|----------------------------------------|--------------------------------------------------------------------------|
| `switchDuration` | all        | 500 ms (`NO`/`NC`), 30 ms (`latching`) | How long to drive the valve at full duty when switching it.              |
| `holdDuty`       | `NO`, `NC` | 100 %                                  | Duty used to hold the valve in its non-default state after switching.    |
| `brakeDuration`  | `latching` | 10 ms                                  | How long to brake the bridge after a pulse, before releasing the driver. |

A latching solenoid only needs its rated pulse (25–30 ms for the S211B and Transol coils).
A longer pulse only heats the coil and loads the supply rail, so a `switchDuration` set
for a motorized valve should not be copied to a latching one.

A latching pulse ends with a brake (both bridge inputs high), not a coast. The coil's
current then decays inside the bridge instead of flying back onto the supply rail.
`brakeDuration` should cover a few L/R time constants of the coil. That is 2–3 ms
for the valves in use, so the 10 ms default leaves margin.
