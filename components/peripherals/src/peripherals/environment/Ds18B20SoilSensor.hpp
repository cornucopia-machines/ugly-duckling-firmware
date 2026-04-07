#pragma once

#include <memory>
#include <optional>
#include <utility>

#include <ds18x20.h>

#include <Configuration.hpp>

#include <peripherals/Peripheral.hpp>
#include <peripherals/api/ITemperatureSensor.hpp>
#include <utils/DebouncedMeasurement.hpp>

#include "Environment.hpp"

using namespace farmhub::kernel;
using namespace farmhub::kernel::mqtt;
using namespace farmhub::peripherals;

namespace farmhub::peripherals::environment {

struct Ds18B20Settings : ConfigurationSection {
    Property<InternalPinPtr> pin { this, "pin" };
    Property<std::string> address { this, "address" };
};

/**
 * @brief Support for DS18B20 soil temperature sensor.
 *
 * Note: Needs a 4.7k pull-up resistor between the data and power lines.
 */
class Ds18B20SoilSensor final
    : public ITemperatureSensor,
      public Peripheral {
public:
    Ds18B20SoilSensor(
        const std::string& name,
        const InternalPinPtr& pin,
        const std::string& address)
        : Peripheral(name)
        , pin(pin) {

        LOGTI(ENV, "Initializing DS18B20 soil temperature sensor '%s' on pin %s",
            name.c_str(), pin->getName().c_str());

        // We rely on the external resistor for pull-up
        gpio_set_pull_mode(pin->getGpio(), GPIO_FLOATING);

        if (!address.empty()) {
            uint64_t parsedAddress = std::strtoull(address.c_str(), nullptr, 16);
            sensor = std::byteswap(parsedAddress);
        } else {
            LOGTV(ENV, "Locating DS18B20 sensors on bus...");
            size_t sensorCount;
            // TODO How many slots do we need here actually?
            int maxSensors = 1;

            esp_err_t searchResult = ds18x20_scan_devices(pin->getGpio(), &sensor, maxSensors, &sensorCount);
            if (searchResult == ESP_OK) {
                if (sensorCount == 0U) {
                    throw PeripheralCreationException("No DS18B20 sensors found on bus");
                }
            } else {
                throw PeripheralCreationException("Error searching for DS18B20 devices: " + std::string(esp_err_to_name(searchResult)));
            }
        }

        LOGTD(ENV, "Using DS18B20 sensor at address: %016llX", sensor);
    }

    Celsius getTemperature() override {
        return measurement.getValue();
    }

private:
    const InternalPinPtr pin;
    onewire_addr_t sensor {};
    utils::DebouncedMeasurement<Celsius> measurement {
        [this](const utils::DebouncedParams<Celsius> _params) -> std::optional<Celsius> {
            float temperature;
            auto err = ds18x20_measure(pin->getGpio(), sensor, false);
            if (err != ESP_OK) {
                LOGTD(ENV, "Error measuring DS18B20 temperature: %s", esp_err_to_name(err));
                return std::nullopt;
            }
            // Wait for conversion (12-bit needs 750ms)
            Task::delay(750ms);
            err = ds18x20_read_temperature(pin->getGpio(), sensor, &temperature);
            if (err != ESP_OK) {
                LOGTD(ENV, "Error reading DS18B20 temperature: %s", esp_err_to_name(err));
                return std::nullopt;
            }
            return temperature;
        },
        1s,
        NAN
    };
};

inline PeripheralFactory makeFactoryForDs18b20() {
    return makePeripheralFactory<ITemperatureSensor, Ds18B20SoilSensor, Ds18B20Settings>(
        "environment:ds18b20",
        "environment",
        [](PeripheralInitParameters& params, const std::shared_ptr<Ds18B20Settings>& settings) {
            auto sensor = std::make_shared<Ds18B20SoilSensor>(
                params.name,
                settings->pin.get(),
                settings->address.get());
            params.registerFeature("temperature", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getTemperature();
            });
            return sensor;
        });
}

}    // namespace farmhub::peripherals::environment
