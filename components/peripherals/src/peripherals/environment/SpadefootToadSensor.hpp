#pragma once
#include <string>

/*
 * Two flavors of Spadefoot Toad soil sensor live in this file:
 *
 *   SpadefootToadSensor               — hardware I2C, used on Spinach (ESP32-S3) and MK11+.
 *   SpadefootToadSensorWithBitbangI2C — software I2C, MK10 (ESP32-C6) only.
 *
 * Why the bitbang variant exists:
 *   ESP32-C6 exposes one fully matrix-routable I2C peripheral (HP_I2C0) and
 *   one pin-locked peripheral (LP_I2C0 on GPIO6/7, which MK10 does not bring
 *   out). MK10 uses HP_I2C0 for the internal bus (GPIO2/3: BQ27220 + INA219).
 *   The external connector (GPIO10/11) cannot reach any remaining hardware I2C
 *   port, so the Spadefoot Toad on that connector is driven in software.
 *
 * Why it lives here rather than as a generic bitbang transport:
 *   This is a scoped fix for one board generation. MK11+ re-routes the
 *   internal bus to LP_I2C0 (GPIO6/7) and keeps HP_I2C0 for the external bus,
 *   so every standard driver works there without bitbang. If a future MK10
 *   firmware ever needs a second non-Spadefoot external I2C device, revisit
 *   whether a general bitbang transport is warranted — do not copy this class.
 */

#include "BitbangI2C.hpp"
#include "Environment.hpp"
#include <EspError.hpp>
#include <I2CManager.hpp>
#include <Result.hpp>
#include <Task.hpp>
#include <peripherals/I2CSettings.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/api/ISoilMoistureSensor.hpp>
#include <peripherals/api/ITemperatureSensor.hpp>
#include <utils/DebouncedMeasurement.hpp>

#include <driver/gpio.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <limits>
#include <memory>
#include <mutex>
#include <vector>

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::peripherals;

namespace cornucopia::ugly_duckling::peripherals::environment {

enum class CalibrationReference : uint8_t {
    Dry,
    Wet,
};

class SpadefootToadSensorSettings
    : public I2CSettings {
public:
    Property<bool> logRawValues { this, "logRawValues", false };
};

struct SpadefootToadReading {
    double moisture = std::numeric_limits<double>::quiet_NaN();
    double temperature = std::numeric_limits<double>::quiet_NaN();
};

// ---------------------------------------------------------------------------
// Base class — all protocol logic, no transport dependency.
// Subclasses implement the three transport primitives.
// ---------------------------------------------------------------------------

class SpadefootToadSensorBase
    : public api::ISoilMoistureSensor,
      public api::ITemperatureSensor,
      public Peripheral {
public:
    Percent getMoisture() override {
        return measurement.getValue().moisture;
    }

    Celsius getTemperature() override {
        return measurement.getValue().temperature;
    }

    Result<> calibrate(CalibrationReference ref) {
        uint8_t cmd = (ref == CalibrationReference::Wet) ? CMD_CALIBRATE_WET : CMD_CALIBRATE_DRY;
        LOGTI(ENV, "Spadefoot Toad '%s': calibrating %s reference",
            getName().c_str(), ref == CalibrationReference::Wet ? "wet" : "dry");
        return transportWriteByte(cmd);
    }

    Result<> readCalibration(JsonObject& res) {
        auto calibration = TRY(transportReadBytes(CMD_READ_CALIBRATION, 17));
        uint8_t csum = 0;
        for (size_t i = 0; i < 16; i++) {
            csum ^= calibration[i];
        }
        if (csum != calibration[16]) {
            res["checksumOk"] = false;
            return {};
        }
        res["checksumOk"] = true;
        static constexpr const char* PROBE_NAMES[] = { "top-front", "top-rear", "bottom-front", "bottom-rear" };
        for (size_t i = 0; i < 4; i++) {
            auto dryVal = static_cast<uint16_t>((calibration[i * 2] << 8) | calibration[(i * 2) + 1]);
            auto wetVal = static_cast<uint16_t>((calibration[8 + (i * 2)] << 8) | calibration[8 + (i * 2) + 1]);
            auto probeObj = res[PROBE_NAMES[i]].to<JsonObject>();
            probeObj["dry"] = dryVal;
            probeObj["wet"] = wetVal;
        }
        return {};
    }

    Result<> factoryReset() {
        LOGTW(ENV, "Spadefoot Toad '%s': factory reset issued", getName().c_str());
        return transportWriteByte(CMD_FACTORY_RESET);
    }

protected:
    // Command bytes are kept in numerical order — please maintain this when adding new entries.
    static constexpr uint8_t CMD_TRIGGER = 0x01;
    static constexpr uint8_t CMD_READ = 0x02;
    static constexpr uint8_t CMD_READ_RAW = 0x03;
    static constexpr uint8_t CMD_CALIBRATE_DRY = 0x04;
    static constexpr uint8_t CMD_CALIBRATE_WET = 0x05;
    static constexpr uint8_t CMD_READ_CALIBRATION = 0x06;
    static constexpr uint8_t CMD_FACTORY_RESET = 0xFB;
    static constexpr uint8_t CMD_GET_FIRMWARE_REV = 0xFC;
    static constexpr uint8_t CMD_GET_DEVICE_REV = 0xFD;
    static constexpr uint8_t CMD_GET_MFR_ID = 0xFE;
    static constexpr uint8_t CMD_GET_DEVICE_ID = 0xFF;

    static constexpr uint16_t EXPECTED_MFR_ID = 0x434D;
    static constexpr uint16_t EXPECTED_DEVICE_ID = 0x0010;

    static constexpr uint8_t FLAG_MOISTURE_VALID = 0x01;
    static constexpr uint8_t FLAG_TEMPERATURE_VALID = 0x02;
    static constexpr uint8_t MOISTURE_INVALID = 0xFF;
    static constexpr int16_t TEMP_INVALID = INT16_MIN;

    SpadefootToadSensorBase(const std::string& name, bool logRawValues)
        : Peripheral(name)
        , logRawValues(logRawValues) {
    }

    // Must be called by the derived class's create() once the transport is ready.
    Result<> initializeProtocol(const std::string& configDescription) {
        LOGTI(ENV, "Initializing Spadefoot Toad soil sensor '%s' with %s",
            getName().c_str(), configDescription.c_str());

        uint16_t mfrId = TRY(transportReadWord(CMD_GET_MFR_ID));
        if (mfrId != EXPECTED_MFR_ID) {
            LOGTE(ENV, "Spadefoot Toad '%s': unexpected manufacturer ID 0x%04X (expected 0x%04X)",
                getName().c_str(), mfrId, EXPECTED_MFR_ID);
            return failure("Spadefoot Toad: manufacturer ID mismatch");
        }

        uint16_t deviceId = TRY(transportReadWord(CMD_GET_DEVICE_ID));
        if (deviceId != EXPECTED_DEVICE_ID) {
            LOGTE(ENV, "Spadefoot Toad '%s': unexpected device ID 0x%04X (expected 0x%04X)",
                getName().c_str(), deviceId, EXPECTED_DEVICE_ID);
            return failure("Spadefoot Toad: device ID mismatch");
        }

        uint16_t firmwareRev = TRY(transportReadWord(CMD_GET_FIRMWARE_REV));
        uint16_t deviceRev = TRY(transportReadWord(CMD_GET_DEVICE_REV));
        LOGTI(ENV, "Spadefoot Toad '%s': firmware rev 0x%04X, device rev 0x%04X",
            getName().c_str(), firmwareRev, deviceRev);

        auto calibration = TRY(transportReadBytes(CMD_READ_CALIBRATION, 17));
        uint8_t calibrationCsum = 0;
        for (size_t i = 0; i < 16; i++) {
            calibrationCsum ^= calibration[i];
        }
        if (calibrationCsum != calibration[16]) {
            LOGTW(ENV, "Spadefoot Toad '%s': calibration checksum mismatch", getName().c_str());
        } else {
            uint16_t dry[4];
            uint16_t wet[4];
            for (size_t i = 0; i < 4; i++) {
                dry[i] = static_cast<uint16_t>((calibration[i * 2] << 8) | calibration[(i * 2) + 1]);
                wet[i] = static_cast<uint16_t>((calibration[8 + (i * 2)] << 8) | calibration[8 + (i * 2) + 1]);
            }
            LOGTI(ENV, "Spadefoot Toad '%s': calibration  TF dry=%u wet=%u  TR dry=%u wet=%u  BF dry=%u wet=%u  BR dry=%u wet=%u",
                getName().c_str(), dry[0], wet[0], dry[1], wet[1], dry[2], wet[2], dry[3], wet[3]);
        }
        return {};
    }

    // Transport primitives — implemented by subclasses.
    // transportReadWord returns the 16-bit value with the first received byte in the high byte.
    virtual Result<> transportWriteByte(uint8_t cmd) = 0;
    virtual Result<uint16_t> transportReadWord(uint8_t cmd) = 0;
    virtual Result<std::vector<uint8_t>> transportReadBytes(uint8_t cmd, size_t n) = 0;

private:
    const bool logRawValues;

    void logRawReading() {
        auto rawResult = transportReadBytes(CMD_READ_RAW, 14);
        if (!rawResult) {
            LOGTW(ENV, "Spadefoot Toad '%s': failed to read raw values: %s",
                getName().c_str(), rawResult.error().message.c_str());
            return;
        }
        const auto& rawData = *rawResult;
        uint8_t rawCsum = 0;
        for (size_t i = 0; i < 13; i++) {
            rawCsum ^= rawData[i];
        }
        if (rawCsum == rawData[13]) {
            uint16_t ticks[4];
            for (size_t i = 0; i < 4; i++) {
                ticks[i] = static_cast<uint16_t>((rawData[i * 2] << 8) | rawData[(i * 2) + 1]);
            }
            auto adcTop = static_cast<uint16_t>((rawData[8] << 8) | rawData[9]);
            auto adcBot = static_cast<uint16_t>((rawData[10] << 8) | rawData[11]);
            LOGTI(ENV, "Spadefoot Toad '%s': raw ticks TF=%u TR=%u BF=%u BR=%u, ADC top=%u bot=%u, flags=0x%02X",
                getName().c_str(), ticks[0], ticks[1], ticks[2], ticks[3], adcTop, adcBot, rawData[12]);
        } else {
            LOGTW(ENV, "Spadefoot Toad '%s': raw read checksum mismatch", getName().c_str());
        }
    }

    utils::DebouncedMeasurement<SpadefootToadReading> measurement {
        [this](const utils::DebouncedParams<SpadefootToadReading>) -> std::optional<SpadefootToadReading> {
            // Phase 1: trigger measurement
            if (auto triggered = transportWriteByte(CMD_TRIGGER); !triggered) {
                LOGTW(ENV, "Spadefoot Toad '%s': failed to trigger measurement: %s",
                    getName().c_str(), triggered.error().message.c_str());
                return std::nullopt;
            }

            // Phase 2: wait 1 s, then read results
            Task::delay(1s);

            // Log raw ADC ticks for debugging (temporary)
            if (logRawValues) {
                logRawReading();
            }

            auto dataResult = transportReadBytes(CMD_READ, 10);
            if (!dataResult) {
                LOGTW(ENV, "Spadefoot Toad '%s': failed to read measurement: %s",
                    getName().c_str(), dataResult.error().message.c_str());
                return std::nullopt;
            }
            const auto& data = *dataResult;

            // Validate checksum (XOR of bytes [0..8])
            uint8_t csum = 0;
            for (size_t i = 0; i < 9; i++) {
                csum ^= data[i];
            }
            if (csum != data[9]) {
                LOGTW(ENV, "Spadefoot Toad '%s': checksum mismatch", getName().c_str());
                return std::nullopt;
            }

            SpadefootToadReading reading;
            uint8_t flags = data[8];

            // Moisture: average of valid probes (flag bit 0 set and value != 0xFF)
            if (flags & FLAG_MOISTURE_VALID) {
                int sum = 0;
                int count = 0;
                for (size_t i = 0; i < 4; i++) {
                    if (data[i] != MOISTURE_INVALID) {
                        sum += data[i];
                        count++;
                    }
                }
                if (count > 0) {
                    reading.moisture = static_cast<double>(sum) / count;
                }
                LOGTI(ENV, "Spadefoot Toad '%s': moisture TF=%d TR=%d BF=%d BR=%d → avg=%.1f%%",
                    getName().c_str(), data[0], data[1], data[2], data[3], reading.moisture);
            }

            // Temperature: average of top and bottom.
            // Requires the temperature valid flag AND both individual readings to be valid.
            if (flags & FLAG_TEMPERATURE_VALID) {
                auto temp_top = static_cast<int16_t>((data[4] << 8) | data[5]);
                auto temp_bot = static_cast<int16_t>((data[6] << 8) | data[7]);
                if (temp_top != TEMP_INVALID && temp_bot != TEMP_INVALID) {
                    reading.temperature = (temp_top + temp_bot) / 20.0;    // two sensors, 0.1 °C units
                    LOGTI(ENV, "Spadefoot Toad '%s': temp top=%.1f°C bot=%.1f°C → avg=%.1f°C",
                        getName().c_str(), temp_top / 10.0, temp_bot / 10.0, reading.temperature);
                } else {
                    LOGTW(ENV, "Spadefoot Toad '%s': temperature valid flag set but individual reading(s) invalid (top=%d bot=%d)",
                        getName().c_str(), temp_top, temp_bot);
                }
            }

            return reading;
        },
        2s
    };
};

// ---------------------------------------------------------------------------
// Hardware I2C variant — used on Spinach (ESP32-S3) and MK11+.
// ---------------------------------------------------------------------------

class SpadefootToadSensor final : public SpadefootToadSensorBase {
public:
    static Result<std::shared_ptr<SpadefootToadSensor>> create(
        const std::string& name,
        const std::shared_ptr<I2CManager>& i2c,
        const I2CConfig& config,
        bool logRawValues) {
        auto device = TRY(i2c->createDevice(name, config));
        auto sensor = std::shared_ptr<SpadefootToadSensor>(new SpadefootToadSensor(name, device, logRawValues));
        TRY(sensor->initializeProtocol(config.toString()));
        return sensor;
    }

protected:
    Result<> transportWriteByte(uint8_t cmd) override {
        return device->writeByte(cmd);
    }

    Result<uint16_t> transportReadWord(uint8_t cmd) override {
        // i2cdev stores bytes in receive order (first byte at low address on LE);
        // swap to put the first received byte (device MSB) in the high byte.
        return device->readRegWord(cmd).transform([](uint16_t word) {
            return __builtin_bswap16(word);
        });
    }

    Result<std::vector<uint8_t>> transportReadBytes(uint8_t cmd, size_t n) override {
        return device->readBytes(cmd, n);
    }

private:
    SpadefootToadSensor(const std::string& name, const std::shared_ptr<I2CDevice>& device, bool logRawValues)
        : SpadefootToadSensorBase(name, logRawValues)
        , device(device) {
    }

    const std::shared_ptr<I2CDevice> device;
};

// ---------------------------------------------------------------------------
// Bitbang I2C variant — MK10 only. See file-level comment.
//
// Open-drain emulation: GPIO_MODE_INPUT_OUTPUT_OD.
// Drive low → write 0. Release → write 1 (external pullup pulls line high).
// Clock: 100 kHz fixed (5 µs half-bit, matching ATtiny TWI Address Match wakeup).
// Clock stretching: poll SCL after release; abort after 10 ms.
// Critical sections: per byte (8 bits + ACK/NACK = ≤90 µs at 100 kHz).
//
// EspBitbangPin and EspBitbangI2CBus (the ESP-IDF adapters) live in
// BitbangI2C.hpp so the state machine and its adapters stay together.
// ---------------------------------------------------------------------------

class SpadefootToadSensorWithBitbangI2C final : public SpadefootToadSensorBase {
public:
    static Result<std::shared_ptr<SpadefootToadSensorWithBitbangI2C>> create(
        const std::string& name,
        const InternalPinPtr& sda,
        const InternalPinPtr& scl,
        uint8_t address,
        bool logRawValues) {
        gpio_config_t conf = {
            .pin_bit_mask = (1ULL << sda->getGpio()) | (1ULL << scl->getGpio()),
            .mode = GPIO_MODE_INPUT_OUTPUT_OD,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_TRY(gpio_config(&conf));
        // Release both lines — bus idle state.
        sda->digitalWrite(1);
        scl->digitalWrite(1);

        auto sensor = std::shared_ptr<SpadefootToadSensorWithBitbangI2C>(new SpadefootToadSensorWithBitbangI2C(name, sda, scl, address, logRawValues));
        char addrBuf[5];
        (void) snprintf(addrBuf, sizeof(addrBuf), "%02X", address);
        TRY(sensor->initializeProtocol("bitbang I2C SDA:" + sda->getName() + " SCL:" + scl->getName() + " addr:0x" + addrBuf));
        return sensor;
    }

protected:
    Result<> transportWriteByte(uint8_t cmd) override {
        std::scoped_lock lock(bbMutex);
        TRY(beginCommand(cmd));
        bus.stop();
        return {};
    }

    Result<uint16_t> transportReadWord(uint8_t cmd) override {
        auto bytes = TRY(transportReadBytes(cmd, 2));
        return static_cast<uint16_t>((bytes[0] << 8) | bytes[1]);
    }

    Result<std::vector<uint8_t>> transportReadBytes(uint8_t cmd, size_t n) override {
        std::scoped_lock lock(bbMutex);
        TRY(beginRead(cmd));
        std::vector<uint8_t> buf(n);
        for (size_t i = 0; i < n; i++) {
            auto byte = bus.readByteAndAck(i == n - 1);    // NACK on last byte
            if (!byte) {
                bus.stop();
                return std::unexpected(std::move(byte).error());
            }
            buf[i] = *byte;
        }
        bus.stop();
        return buf;
    }

private:
    SpadefootToadSensorWithBitbangI2C(
        const std::string& name,
        const InternalPinPtr& sda,
        const InternalPinPtr& scl,
        uint8_t address,
        bool logRawValues)
        : SpadefootToadSensorBase(name, logRawValues)
        , sdaPin(sda)
        , sclPin(scl)
        , bus(sdaPin, sclPin)
        , bbAddress(address) {
    }

    // Writes a byte, failing on NACK; stops the bus on any failure.
    Result<> writeByteExpectAck(uint8_t byte, const char* what) {
        auto ack = bus.writeByteGetAck(byte);
        if (!ack) {
            bus.stop();
            return std::unexpected(std::move(ack).error());
        }
        if (!*ack) {
            bus.stop();
            return failure(std::string("Spadefoot Toad bitbang: NACK on ") + what);
        }
        return {};
    }

    // START, address for writing, then the command byte; the bus is left open on success.
    Result<> beginCommand(uint8_t cmd) {
        bus.start();
        TRY(writeByteExpectAck(static_cast<uint8_t>((bbAddress << 1) | 0x00), "address (write)"));
        return writeByteExpectAck(cmd, "command byte");
    }

    // Sends the command, then a REPEATED START and the address for reading.
    Result<> beginRead(uint8_t cmd) {
        TRY(beginCommand(cmd));
        bus.repeatedStart();
        return writeByteExpectAck(static_cast<uint8_t>((bbAddress << 1) | 0x01), "address (read)");
    }

    // Members are declared in initialization order: pins first, then bus (holds refs to pins).
    EspBitbangPin sdaPin;
    EspBitbangPin sclPin;
    EspBitbangI2CBus bus;
    const uint8_t bbAddress;
    std::mutex bbMutex;
};

// ---------------------------------------------------------------------------
// Factory functions
// ---------------------------------------------------------------------------

inline void reportCommandError(const Result<>& result, JsonObject& res) {
    if (!result) {
        LOGTE(ENV, "Spadefoot Toad command failed: %s", result.error().message.c_str());
        res["error"] = result.error().message;
    }
}

inline void registerFeaturesAndCommands(PeripheralInitParameters& params, const std::shared_ptr<SpadefootToadSensorBase>& sensor) {
    params.registerFeature("moisture", [sensor](JsonObject& telemetryJson) {
        telemetryJson["value"] = sensor->getMoisture();
    });
    params.registerFeature("temperature", [sensor](JsonObject& telemetryJson) {
        telemetryJson["value"] = sensor->getTemperature();
    });
    params.peripheralRoot()->registerCommand("calibrate", [sensor](const JsonObject& req, JsonObject& res) {
        auto ref = req["reference"].as<CalibrationReference>();
        reportCommandError(sensor->calibrate(ref), res);
    });
    params.peripheralRoot()->registerCommand("read-calibration", [sensor](const JsonObject&, JsonObject& res) {
        reportCommandError(sensor->readCalibration(res), res);
    });
    params.peripheralRoot()->registerCommand("factory-reset", [sensor](const JsonObject&, JsonObject& res) {
        reportCommandError(sensor->factoryReset(), res);
    });
}

inline PeripheralFactory makeFactoryForSpadefootToadSensor() {
    return makePeripheralFactory<
        SpadefootToadSensor,
        SpadefootToadSensorSettings,
        api::ISoilMoistureSensor,
        api::ITemperatureSensor>(
        "soil:spadefoot-toad",
        "environment",
        [](PeripheralInitParameters& params, const std::shared_ptr<SpadefootToadSensorSettings>& settings) -> Result<std::shared_ptr<SpadefootToadSensor>> {
            I2CConfig i2cConfig = settings->parse(0x20);
            i2cConfig.clkSpeed = 100000;    // ATtiny TWI Address Match wakeup requires 100 kHz
            auto sensor = TRY(SpadefootToadSensor::create(params.name, params.services.i2c, i2cConfig, settings->logRawValues.get()));
            registerFeaturesAndCommands(params, sensor);
            return sensor;
        });
}

// MK10-only: bitbang I2C variant for the external bus. See file-level comment.
inline PeripheralFactory makeFactoryForSpadefootToadSensorWithBitbangI2C() {
    return makePeripheralFactory<
        SpadefootToadSensorWithBitbangI2C,
        SpadefootToadSensorSettings,
        api::ISoilMoistureSensor,
        api::ITemperatureSensor>(
        "soil:spadefoot-toad-bb",
        "environment",
        [](PeripheralInitParameters& params, const std::shared_ptr<SpadefootToadSensorSettings>& settings) -> Result<std::shared_ptr<SpadefootToadSensorWithBitbangI2C>> {
            // No i2c service — raw pin pointers are passed directly.
            // Clock is fixed at 100 kHz by the bitbang transport.
            I2CConfig i2cConfig = settings->parse(0x20);
            auto sensor = TRY(SpadefootToadSensorWithBitbangI2C::create(params.name, i2cConfig.sda, i2cConfig.scl, i2cConfig.address, settings->logRawValues.get()));
            registerFeaturesAndCommands(params, sensor);
            return sensor;
        });
}

}    // namespace cornucopia::ugly_duckling::peripherals::environment

namespace ArduinoJson {

using cornucopia::ugly_duckling::peripherals::environment::CalibrationReference;

template <>
struct Converter<CalibrationReference> {
    static bool toJson(const CalibrationReference& src, JsonVariant dst) {
        switch (src) {
            case CalibrationReference::Dry:
                return dst.set("dry");
            case CalibrationReference::Wet:
                return dst.set("wet");
        }
        return false;
    }

    static CalibrationReference fromJson(JsonVariantConst src) {
        auto s = src.as<std::string>();
        return (s == "wet")
            ? CalibrationReference::Wet
            : CalibrationReference::Dry;    // default / unknown → Dry
    }

    static bool checkJson(JsonVariantConst src) {
        if (!src.is<const char*>()) {
            return false;
        }
        auto s = src.as<std::string>();
        return s == "dry" || s == "wet";
    }
};

}    // namespace ArduinoJson
