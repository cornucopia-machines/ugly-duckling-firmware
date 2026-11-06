# Peripherals

How to connect and configure the peripherals Ugly Duckling supports. For what peripherals,
features and functions are, see [`Components.md`](../Components.md); for how configuration
reaches the device, see [`Configuration.md`](../Configuration.md).

Not every supported peripheral is documented here yet; pages are added as peripherals are added
or revisited.

| Peripheral | Type | Measures / controls | Connection |
| --- | --- | --- | --- |
| [Truebner SMT100](SMT100.md) | `soil:smt100` | Soil moisture and temperature | I²C, through a Mole Cricket UART bridge (LV UART variant) |
