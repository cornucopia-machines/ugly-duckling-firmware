#pragma once

/*
 * ZTS-3002-TR-ECTHNPK-N01 soil multi-parameter sensor (moisture, temperature, conductivity, and the
 * N/P/K estimates the sensor derives from conductivity) on an RS485 bus, as a peripheral.
 *
 * The probe is powered from the switched rail with the bus (MK14), so every reading waits out the
 * probe's start-up time since the rail came on.
 */

#include "Environment.hpp"
#include "ZtsSoilProtocol.hpp"
#include <Result.hpp>
#include <config/Configuration.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/api/ISoilMoistureSensor.hpp>
#include <peripherals/api/ITemperatureSensor.hpp>
#include <peripherals/serial/SerialPort.hpp>
#include <utils/DebouncedMeasurement.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::peripherals;
using namespace cornucopia::ugly_duckling::peripherals::serial;

namespace cornucopia::ugly_duckling::peripherals::environment {

class ZtsSoilSettings
    : public ConfigurationSection {
public:
    // Modbus address of the probe, 1 to 247; probes sharing the bus need different ones
    Property<int> address { this, "address", ZtsSoilProtocol::DEFAULT_ADDRESS };
    Property<int> baud { this, "baud", static_cast<int>(ZtsSoilProtocol::DEFAULT_BAUD) };
    // Also publish nitrogen, phosphorus and potassium. The probe doesn't measure them: unless
    // calibrated, it derives them from conductivity.
    Property<bool> npk { this, "npk", false };
    // Wait after powering the probe before querying it. The ZTS manual gives 1 s until the readings
    // are stable; another manual for the same probe family gives up to 5 minutes
    Property<milliseconds> startupTime { this, "startupTime", 1000ms };
    // Wait for each response. A full 7-register response takes ~40 ms on the wire at 4800 baud
    Property<milliseconds> responseTimeout { this, "responseTimeout", 500ms };
};

class ZtsSoilSensor final
    : public api::ISoilMoistureSensor,
      public api::ITemperatureSensor,
      public Peripheral {
public:
    ZtsSoilSensor(const std::string& name, std::shared_ptr<ISerialLink> link, std::unique_ptr<ZtsSoilProtocol> protocol, milliseconds startupTime)
        : Peripheral(name)
        , link(std::move(link))
        , protocol(std::move(protocol))
        , startupTime(startupTime) {
    }

    Percent getMoisture() override {
        return measurement.getValue().moisture;
    }

    Celsius getTemperature() override {
        return measurement.getValue().temperature;
    }

    ZtsSoilReading getReading() {
        return measurement.getValue();
    }

    std::string describe() const {
        return link->describe() + ", " + protocol->describe();
    }

private:
    Result<ZtsSoilReading> readProbe() {
        auto session = TRY(link->open());
        session->waitPoweredFor(startupTime);
        // Discard whatever the probe sent while starting up
        TRY(session->flushRx());
        return protocol->read(*session);
    }

    const std::shared_ptr<ISerialLink> link;
    const std::unique_ptr<ZtsSoilProtocol> protocol;
    const milliseconds startupTime;

    utils::DebouncedMeasurement<ZtsSoilReading> measurement {
        [this](const utils::DebouncedParams<ZtsSoilReading>) -> std::optional<ZtsSoilReading> {
            auto reading = readProbe();
            if (!reading) {
                LOGTW(ZTS_SOIL, "ZTS soil sensor '%s' (%s): failed to read: %s",
                    getName().c_str(), describe().c_str(), reading.error().message.c_str());
                // Report the failure as missing values rather than repeating the last good reading
                return ZtsSoilReading {};
            }
            LOGTI(ZTS_SOIL, "ZTS soil sensor '%s': moisture %.1f%%, temperature %.1f °C, conductivity %.0f µS/cm",
                getName().c_str(), reading->moisture, reading->temperature, reading->conductivity);
            return *reading;
        },
        // All values come from one Modbus read; this lets one telemetry pass use one reading
        2s
    };
};

/**
 * @param rs485 Returns the board's RS485 bus, setting it up on first use.
 */
inline PeripheralFactory makeFactoryForZtsSoilSensor(SerialBusProvider rs485) {
    return makePeripheralFactory<
        ZtsSoilSensor,
        ZtsSoilSettings,
        api::ISoilMoistureSensor,
        api::ITemperatureSensor>(
        "soil:zts-3002",
        "environment",
        [rs485 = std::move(rs485)](PeripheralInitParameters& params, const std::shared_ptr<ZtsSoilSettings>& settings) -> Result<std::shared_ptr<ZtsSoilSensor>> {
            int address = settings->address.get();
            if (address < 1 || address > 247) {
                return failure("ZTS soil sensor address must be 1 to 247, got " + std::to_string(address));
            }
            int baud = settings->baud.get();
            if (baud != 2400 && baud != 4800 && baud != 9600) {
                return failure("ZTS soil sensor baud rate must be 2400, 4800 or 9600, got " + std::to_string(baud));
            }
            bool npk = settings->npk.get();
            auto bus = TRY(rs485());
            auto sensor = std::make_shared<ZtsSoilSensor>(
                params.name,
                bus->link(static_cast<uint32_t>(baud)),
                std::make_unique<ZtsSoilProtocol>(static_cast<uint8_t>(address), npk, settings->responseTimeout.get()),
                settings->startupTime.get());
            LOGTI(ZTS_SOIL, "ZTS soil sensor '%s' on %s", params.name.c_str(), sensor->describe().c_str());
            params.registerFeature("moisture", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getMoisture();
            });
            params.registerFeature("temperature", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getTemperature();
            });
            params.registerFeature("conductivity", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getReading().conductivity;
            });
            if (npk) {
                params.registerFeature("nitrogen", [sensor](JsonObject& telemetryJson) {
                    telemetryJson["value"] = sensor->getReading().nitrogen;
                });
                params.registerFeature("phosphorus", [sensor](JsonObject& telemetryJson) {
                    telemetryJson["value"] = sensor->getReading().phosphorus;
                });
                params.registerFeature("potassium", [sensor](JsonObject& telemetryJson) {
                    telemetryJson["value"] = sensor->getReading().potassium;
                });
            }
            return sensor;
        });
}

}    // namespace cornucopia::ugly_duckling::peripherals::environment
