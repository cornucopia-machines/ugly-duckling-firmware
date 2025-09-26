#pragma once

#include <memory>
#include <utility>

#include <I2CManager.hpp>
#include <peripherals/I2CSettings.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/api/ISoilMoistureSensor.hpp>
#include <utils/DebouncedMeasurement.hpp>

using namespace farmhub::kernel;
using namespace farmhub::peripherals;

namespace farmhub::peripherals::environment {

class Fdc1004SoilMoistureSensorSettings
    : public I2CSettings {
public:
};

class Fdc1004SoilMoistureSensor final
    : public api::ISoilMoistureSensor,
      public Peripheral {
public:
    Fdc1004SoilMoistureSensor(
        const std::string& name,
        const std::shared_ptr<I2CManager>& i2c,
        const I2CConfig& config)
        : Peripheral(name)
        , device(i2c->createDevice(name, config)) {

        LOGTI(ENV, "Initializing FDC1004 soil moisture sensor '%s' with %s",
            name.c_str(), config.toString().c_str());

        auto manufacturerId = device->readRegWord(0xFE);
        auto deviceId = device->readRegWord(0xFF);

        LOGTD(ENV, "FDC1004 Manufacturer ID: 0x%04x, Device ID: 0x%04x",
            manufacturerId, deviceId);
    }

    Percent getMoisture() override {
        return measurement.getValue();
    }

private:
    std::shared_ptr<I2CDevice> device;

    // Measurement index 1..4 maps to register groups
    enum class Meas : uint8_t {
        M1 = 1,
        M2,
        M3,
        M4,
    };

    // Positive input CINx for CHA
    enum class Cin : uint8_t {
        CIN1 = 0,
        CIN2 = 1,
        CIN3 = 2,
        CIN4 = 3,
    };

    // Sample rates per datasheet (FDC_CONF RATE bits [11:10])
    enum class Rate : uint16_t {
        SPS_100 = 0b01,
        SPS_200 = 0b10,
        SPS_400 = 0b11,
    };

    utils::DebouncedMeasurement<Percent> measurement {
        [this](const utils::DebouncedParams<Percent> params) -> std::optional<Percent> {
            return std::nullopt;
        },
        1s,
        NAN
    };
};

inline PeripheralFactory makeFactoryForFdc1004SoilMoisture() {
    return makePeripheralFactory<ISoilMoistureSensor, Fdc1004SoilMoistureSensor, Fdc1004SoilMoistureSensorSettings>(
        "environment:fdc1004-soil-moisture",
        "environment",
        [](PeripheralInitParameters& params, const std::shared_ptr<Fdc1004SoilMoistureSensorSettings>& settings) {
            I2CConfig i2cConfig = settings->parse(0x50);    // Fixed address for FDC1004
            auto sensor = std::make_shared<Fdc1004SoilMoistureSensor>(
                params.name,
                params.services.i2c,
                i2cConfig);
            params.registerFeature("moisture", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getMoisture();
            });
            return sensor;
        });
}

}    // namespace farmhub::peripherals::environment
