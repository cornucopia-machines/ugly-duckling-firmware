#pragma once

/*
 * Truebner SMT100 soil probe (volumetric water content and temperature) as a peripheral.
 *
 * The probe sits on a serial link: the LV UART variant on a Mole Cricket board (SC16IS752
 * I²C-to-UART bridge) on the I²C port, or the RS485 variant directly on the board's RS485 port
 * (MK14). The protocol is pluggable (ISmt100Protocol); only ASCII is implemented.
 *
 * Every reading is a cold start: the link powers the probe, we wait for it to start up, query it,
 * and the link powers it down again.
 */

#include "Environment.hpp"
#include "Smt100Protocol.hpp"
#include <I2CManager.hpp>
#include <Result.hpp>
#include <peripherals/I2CSettings.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/api/ISoilMoistureSensor.hpp>
#include <peripherals/api/ITemperatureSensor.hpp>
#include <peripherals/serial/MoleCricketRegistry.hpp>
#include <peripherals/serial/SerialPort.hpp>
#include <utils/DebouncedMeasurement.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::peripherals;
using namespace cornucopia::ugly_duckling::peripherals::serial;

namespace cornucopia::ugly_duckling::peripherals::environment {

// The I²C settings (sda, scl, address, clkSpeed) and channel only apply to "mole-cricket"
class Smt100Settings
    : public I2CSettings {
public:
    // How the probe connects: "mole-cricket" (LV UART variant) or "rs485" (RS485 variant)
    Property<std::string> bus { this, "bus", "mole-cricket" };
    // Mole Cricket terminal: "a" or "b"
    Property<std::string> channel { this, "channel", "a" };
    // Probe address; 000000 is broadcast, fine with one probe per line (Mole Cricket terminal or
    // RS485 bus)
    Property<std::string> probeAddress { this, "probeAddress", Smt100AsciiProtocol::BROADCAST_ADDRESS };
    // Only "ascii" for now; "modbus" may follow
    Property<std::string> protocol { this, "protocol", "ascii" };
    // Wait after powering the probe before querying it. The manual gives < 500 ms until stable;
    // 100 ms worked on the bench, but we keep the manual's margin
    Property<milliseconds> startupTime { this, "startupTime", 500ms };
    // Wait for each response. GetTemperature took 330 ms on the bench, and 200 ms was too short
    Property<milliseconds> responseTimeout { this, "responseTimeout", 500ms };
};

class Smt100Sensor final
    : public api::ISoilMoistureSensor,
      public api::ITemperatureSensor,
      public Peripheral {
public:
    Smt100Sensor(const std::string& name, std::shared_ptr<ISerialLink> link, std::unique_ptr<ISmt100Protocol> protocol, milliseconds startupTime)
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

    // Logs the probe's identity. A probe that doesn't answer is not fatal: it may be plugged in later.
    void logIdentity() {
        auto identity = withProbe([this](ISerialPort& port) { return protocol->identify(port); });
        if (identity) {
            LOGTI(SMT100, "SMT100 '%s' on %s (%s): firmware %s",
                getName().c_str(), link->describe().c_str(), protocol->describe().c_str(), identity->c_str());
        } else {
            LOGTW(SMT100, "SMT100 '%s' on %s (%s) did not identify itself: %s",
                getName().c_str(), link->describe().c_str(), protocol->describe().c_str(), identity.error().message.c_str());
        }
    }

private:
    // Powers the probe, waits for it to start up, runs `action`, and powers the probe down again
    template <typename Action>
    auto withProbe(Action action) -> decltype(action(std::declval<ISerialPort&>())) {
        auto session = TRY(link->open());
        auto start = steady_clock::now();
        session->waitPoweredFor(startupTime);
        // Discard whatever the probe sent while starting up
        TRY(session->flushRx());
        auto result = action(*session);
        LOGTD(SMT100, "SMT100 '%s': probe powered for %lld ms",
            getName().c_str(), static_cast<long long>(duration_cast<milliseconds>(steady_clock::now() - start).count()));
        return result;
    }

    const std::shared_ptr<ISerialLink> link;
    const std::unique_ptr<ISmt100Protocol> protocol;
    const milliseconds startupTime;

    utils::DebouncedMeasurement<Smt100Reading> measurement {
        [this](const utils::DebouncedParams<Smt100Reading>) -> std::optional<Smt100Reading> {
            auto reading = withProbe([this](ISerialPort& port) { return protocol->read(port); });
            if (!reading) {
                LOGTW(SMT100, "SMT100 '%s': failed to read: %s",
                    getName().c_str(), reading.error().message.c_str());
                // Report the failure as missing values rather than repeating the last good reading
                return Smt100Reading {};
            }
            LOGTI(SMT100, "SMT100 '%s': moisture %.1f%%, temperature %.1f °C",
                getName().c_str(), reading->moisture, reading->temperature);
            return *reading;
        },
        // Moisture and temperature are read together; this lets one telemetry pass use one reading
        2s
    };
};

inline Result<MoleCricket::Channel> parseMoleCricketChannel(const std::string& channel) {
    if (channel == "a" || channel == "A") {
        return MoleCricket::Channel::A;
    }
    if (channel == "b" || channel == "B") {
        return MoleCricket::Channel::B;
    }
    return failure("Mole Cricket channel must be 'a' or 'b', got '" + channel + "'");
}

// The SMT100's serial line runs at a fixed 9600 baud, on either variant
constexpr uint32_t SMT100_BAUD = 9600;

inline Result<std::shared_ptr<ISerialLink>> createSmt100Link(
    PeripheralInitParameters& params,
    const Smt100Settings& settings,
    MoleCricketRegistry& moleCrickets,
    const SerialBusProvider& rs485) {
    const auto& bus = settings.bus.get();
    if (bus == "rs485") {
        auto rs485Bus = TRY(rs485());
        return rs485Bus->link(SMT100_BAUD);
    }
    if (bus == "mole-cricket") {
        I2CConfig i2cConfig = settings.parse(0x48);
        // Mole Cricket hangs off the port on a ~1 m cable; its design targets 100 kHz
        i2cConfig.clkSpeed = 100000;
        auto channel = TRY(parseMoleCricketChannel(settings.channel.get()));
        auto board = TRY(moleCrickets.getOrCreate(params.services.i2c, params.services.switchedRail, i2cConfig));
        return board->link(channel);
    }
    return failure("SMT100 bus must be 'mole-cricket' or 'rs485', got '" + bus + "'");
}

inline Result<std::unique_ptr<ISmt100Protocol>> createSmt100Protocol(const Smt100Settings& settings) {
    if (settings.protocol.get() == "ascii") {
        return TRY(Smt100AsciiProtocol::create(settings.probeAddress.get(), settings.responseTimeout.get()));
    }
    return failure("Unsupported SMT100 protocol '" + settings.protocol.get() + "'; supported: ascii");
}

/**
 * @param rs485 Returns the board's RS485 bus, setting it up on first use.
 */
inline PeripheralFactory makeFactoryForSmt100(SerialBusProvider rs485) {
    auto registry = std::make_shared<MoleCricketRegistry>();
    return makePeripheralFactory<
        Smt100Sensor,
        Smt100Settings,
        api::ISoilMoistureSensor,
        api::ITemperatureSensor>(
        "soil:smt100",
        "environment",
        [registry, rs485 = std::move(rs485)](PeripheralInitParameters& params, const std::shared_ptr<Smt100Settings>& settings) -> Result<std::shared_ptr<Smt100Sensor>> {
            auto protocol = TRY(createSmt100Protocol(*settings));
            auto link = TRY(createSmt100Link(params, *settings, *registry, rs485));
            auto sensor = std::make_shared<Smt100Sensor>(params.name, std::move(link), std::move(protocol), settings->startupTime.get());
            sensor->logIdentity();
            params.registerFeature("moisture", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getMoisture();
            });
            params.registerFeature("temperature", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getTemperature();
            });
            return sensor;
        });
}

}    // namespace cornucopia::ugly_duckling::peripherals::environment
