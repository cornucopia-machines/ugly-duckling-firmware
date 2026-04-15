# Chirp Soil Sensor Implementation Plan

## Overview

Add support for the [Chirp!](https://wemakethings.net/chirp/) I2C soil sensor, which provides both soil moisture (capacitance) and temperature readings. This also involves renaming the existing analog `SoilMoistureSensor` to `Hw390SoilMoistureSensor` to disambiguate it.

## Chirp I2C Protocol

- **Default I2C address:** `0x20`
- **Registers:**
  - `0x00` - Capacitance (moisture), 16-bit big-endian, read
  - `0x03` - Request light measurement, write (command)
  - `0x04` - Light level, 16-bit big-endian, read
  - `0x05` - Temperature, 16-bit big-endian, read
  - `0x06` - Reset, write (command)
- **Temperature** is returned in 1/10 degree Celsius (e.g. 256 = 25.6 C)
- **Capacitance** is a raw value; needs calibration per-sensor (air/water values, like Hw390)
- **Timing:** The device enters I2C sensor mode (no chirping) if any I2C communication occurs during its initial measurement period after reset. Light measurement needs ~9s in darkness; capacitance and temperature can be read with minimal delay.

## Step 1: Rename `SoilMoistureSensor` to `Hw390SoilMoistureSensor`

Rename the file and all internal references to make room for other soil sensor implementations.

### Files to change

- `components/peripherals/src/peripherals/environment/SoilMoistureSensor.hpp` -- rename file to `Hw390SoilMoistureSensor.hpp`; rename class to `Hw390SoilMoistureSensor`, settings to `Hw390SoilMoistureSensorSettings`, factory function to `makeFactoryForHw390SoilMoisture()`
- `components/devices/src/devices/DeviceDefinition.hpp` -- update include and `registerFactory()` call

The canonical factory type becomes `soil:hw390`. To keep backward compatibility with `environment:soil-moisture`, we register the same factory a second time under the old name -- there is no alias mechanism in `Manager`, but `registerFactory()` can simply be called twice with distinct `factoryType` strings pointing to the same create logic.

## Step 2: Create `ChirpSoilSensor`

New file: `components/peripherals/src/peripherals/environment/ChirpSoilSensor.hpp`

### Design

- **Interfaces:** Implement both `api::ISoilMoistureSensor` and `api::ITemperatureSensor`, plus `Peripheral`
- **I2C communication:** Use `I2CDevice` from `I2CManager.hpp` directly (no external driver library needed -- the protocol is simple enough)
- **Settings:** Extend `I2CSettings` with calibration values (`air`, `water`) similar to HW-390
- **Measurement:** Use `DebouncedMeasurement` or a manual throttle (like Sht3xSensor) to avoid excessive I2C reads. A 1-second debounce is reasonable.

### Class sketch

```cpp
class ChirpSoilSensorSettings
    : public I2CSettings {
public:
    // Raw capacitance calibration values
    Property<uint16_t> air { this, "air", 300 };
    Property<uint16_t> water { this, "water", 700 };
};

class ChirpSoilSensor final
    : public api::ISoilMoistureSensor,
      public api::ITemperatureSensor,
      public Peripheral {
public:
    ChirpSoilSensor(
        const std::string& name,
        const std::shared_ptr<I2CManager>& i2c,
        const I2CConfig& config,
        int airValue,
        int waterValue);

    Percent getMoisture() override;
    Celsius getTemperature() override;

private:
    void updateMeasurement();  // Reads registers 0x00 and 0x05

    std::shared_ptr<I2CDevice> device;
    const int airValue;
    const int waterValue;

    Mutex mutex;
    steady_clock::time_point lastMeasurementTime;
    double moisture = NAN;
    double temperature = NAN;
};
```

### I2C read logic

```cpp
void updateMeasurement() {
    auto now = steady_clock::now();
    if (now - lastMeasurementTime < 1s) return;

    // Read capacitance (register 0x00), 16-bit big-endian
    uint16_t rawMoisture = device->readRegWord(0x00);
    rawMoisture = __builtin_bswap16(rawMoisture);  // Convert from big-endian

    // Read temperature (register 0x05), 16-bit big-endian, units of 1/10 C
    uint16_t rawTemp = device->readRegWord(0x05);
    rawTemp = __builtin_bswap16(rawTemp);
    temperature = rawTemp / 10.0;

    // Convert capacitance to percentage
    double run = waterValue - airValue;
    moisture = ((rawMoisture - airValue) * 100.0) / run;

    lastMeasurementTime = now;
}
```

**Note:** `I2CDevice::readRegWord()` reads 2 bytes in wire order. The Chirp sends MSB first (big-endian), but ESP32 is little-endian, so we need a byte swap. We should verify this during testing -- if `readRegWord` already returns the value in the correct host order we can drop the swap.

### Factory function

```cpp
inline PeripheralFactory makeFactoryForChirpSoilSensor() {
    return makePeripheralFactory<ChirpSoilSensor, ChirpSoilSensorSettings, ISoilMoistureSensor, ITemperatureSensor>(
        "soil:chirp",
        "environment",
        [](PeripheralInitParameters& params, const std::shared_ptr<ChirpSoilSensorSettings>& settings) {
            I2CConfig i2cConfig = settings->parse(0x20);
            auto sensor = std::make_shared<ChirpSoilSensor>(
                params.name,
                params.services.i2c,
                i2cConfig,
                settings->air.get(),
                settings->water.get());
            params.registerFeature("moisture", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getMoisture();
            });
            params.registerFeature("temperature", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getTemperature();
            });
            return sensor;
        });
}
```

Following the `Sht3xSensor` pattern: use the concrete class as the first template argument (since we implement multiple interfaces), register separate `"moisture"` and `"temperature"` telemetry features.

## Step 3: Register the factory

In `DeviceDefinition.hpp`:

- Add `#include <peripherals/environment/ChirpSoilSensor.hpp>`
- Add `peripheralManager->registerFactory(environment::makeFactoryForChirpSoilSensor());`

## JSON configuration example

```json
{
  "type": "soil:chirp",
  "name": "soil",
  "params": {
    "address": "0x20",
    "sda": "GPIO4",
    "scl": "GPIO5",
    "air": 300,
    "water": 700
  }
}
```

## Open questions

1. **Byte order:** Need to verify whether `I2CDevice::readRegWord()` already handles endianness or returns raw wire bytes. Test with actual hardware.
2. **Light sensor:** The Chirp also has a light sensor (registers 0x03/0x04). We're skipping it for now but could add `ILightSensor` support later.
3. **Error handling:** Return NaN when the I2C read fails, consistent with Sht3xSensor.
