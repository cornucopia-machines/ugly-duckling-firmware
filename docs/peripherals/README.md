# Peripherals

How to connect and configure the peripherals Ugly Duckling supports. For what peripherals,
features and functions are, see [`Components.md`](../Components.md); for how configuration
reaches the device, see [`Configuration.md`](../Configuration.md).

## Sensor power on MK14

From MK14 on, the I²C and RS485 ports are only powered while a peripheral is using them. It shares its power
switch (LOADEN) with the valve driver and its boost converter, so a reading powers those up too.
A sensor loses its settings whenever the port goes off, so after each power-up the firmware waits
for the sensor to start up and sets it up again before reading it. All sensors read in one
telemetry pass share a single power-up. Earlier boards power the port all the time.

The start-up wait and setup of each sensor are listed under
[sensor power](../SensorPower.md#writing-a-driver-for-a-port-sensor).

## Actuators

| Peripheral | Type | Measures / controls | Connection |
| --- | --- | --- | --- |
| [Valve](Valve.md) | `valve` | Opens and closes a water valve (latching, NC or NO) | Motor driver output on the board, or a pin |
| [Door](Door.md) | `door` | Moves a motorized door between open and closed end switches | Motor driver output on the board, plus two GPIO end-switch inputs |

## Soil

| Peripheral | Type | Measures / controls | Connection |
| --- | --- | --- | --- |
| [Spadefoot Toad](SpadefootToad.md) | `soil:spadefoot-toad`, `soil:spadefoot-toad-bb` | Soil moisture and temperature (two depths, averaged) | I²C, 100 kHz (`-bb`: software I²C, MK10 only) |
| [Truebner SMT100](SMT100.md) | `soil:smt100` | Soil moisture and temperature | I²C, through a Mole Cricket UART bridge (LV UART variant); or RS485 (RS485 variant, MK14 only) |
| [ZTS-3002](ZTS-3002.md) | `soil:zts-3002` | Soil moisture, temperature and conductivity (N/P/K estimates optional) | RS485, Modbus RTU (MK14 only) |
| [Chirp](Chirp.md) | `soil:chirp` | Soil moisture and temperature | I²C |
| [HW-390](HW-390.md) | `soil:hw390`, `environment:soil-moisture` | Soil moisture | Analog (ADC pin), optional disable pin |
| [DS18B20](DS18B20.md) | `environment:ds18b20` | Soil temperature | 1-Wire (GPIO, external 4.7 kΩ pull-up) |
| [Kalman-filtered soil moisture](KalmanSoilMoisture.md) | `environment:kalman-soil-moisture` | Temperature-compensated soil moisture | Virtual: wraps a moisture sensor and a temperature sensor |

## Air

| Peripheral | Type | Measures / controls | Connection |
| --- | --- | --- | --- |
| [Sensirion SHT3x](SHT3x.md) | `environment:sht3x` | Air temperature and relative humidity | I²C (default address 0x44, or 0x45) |
| [SHT2x / HTU2x](SHT2x.md) | `environment:sht2x`, `environment:htu2x` | Air temperature and relative humidity | I²C (fixed address 0x40) |
| [TI HDC2010](HDC2010.md) | `environment:hdc2010` | Air temperature and relative humidity | I²C (default address 0x40, or 0x41) |
| [NTC thermistor](NTC.md) | `environment:ntc-temperature-sensor` | Temperature | Analog (ADC pin), voltage divider |

## Light

| Peripheral | Type | Measures / controls | Connection |
| --- | --- | --- | --- |
| [BH1750](BH1750.md) | `light-sensor:bh1750` | Ambient light (lux) | I²C (default address 0x23, or 0x5C) |
| [TSL2591](TSL2591.md) | `light-sensor:tsl2591` | Ambient light (lux) | I²C (fixed address 0x29) |
| [Analog light sensor](AnalogLightSensor.md) | `light-sensor:analog` | Ambient light (lux) from a photoresistor; Wokwi simulation only | Analog (ADC) pin |

## Water and other meters

| Peripheral | Type | Measures / controls | Connection |
| --- | --- | --- | --- |
| [Flow meter](FlowMeter.md) | `flow-meter` | Water volume (l) and flow rate (l/min) from a pulse-output meter | Pulse input on an internal GPIO (`IFLOWA`/`IFLOWB` on MK9 and later) |
| [Analog meter](AnalogMeter.md) | `analog-meter` | Any analog voltage sensor (e.g. pressure), linearly scaled | ADC-capable internal GPIO |
| [Electric fence monitor](ElectricFence.md) | `electric-fence` | Fence voltage level (V), from pulses on per-voltage pins; not deployed yet | Pulse inputs on internal GPIOs |
