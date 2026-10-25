#pragma once
#include <Pin.hpp>
#include <config/Configuration.hpp>

#include <memory>
#include <string>
#include <utility>

namespace cornucopia::ugly_duckling::peripherals::multiplexer {

class Xl9535Settings
    : public I2CSettings {
};

class Xl9535 final
    : public Peripheral {
public:
    static Result<std::shared_ptr<Xl9535>> create(
        const std::string& name,
        const std::shared_ptr<I2CManager>& i2c,
        const I2CConfig& config) {
        auto device = TRY(i2c->createDevice(name, config));

        LOGI("Initializing XL9535 multiplexer '%s' with %s",
            name.c_str(), config.toString().c_str());
        return std::shared_ptr<Xl9535>(new Xl9535(name, device));
    }

    void pinMode(uint8_t pin, Pin::Mode mode) {
        // TODO Signal if pull-up or pull-down is requested that we cannot support it
        if (mode == Pin::Mode::Output) {
            direction &= static_cast<uint16_t>(~bit(pin));
        } else {
            direction |= bit(pin);
        }
        if (pin < 8) {
            updateDirection1();
        } else {
            updateDirection2();
        }
    }

    void digitalWrite(uint8_t pin, uint8_t val) {
        if (val == 1) {
            output |= bit(pin);
        } else {
            output &= static_cast<uint16_t>(~bit(pin));
        }
        if (pin < 8) {
            updateOutput1();
        } else {
            updateOutput2();
        }
    }

    /**
     * @return the pin's level, or 0 if it could not be read.
     */
    int digitalRead(uint8_t pin) {
        auto data = device->readRegByte(pin < 8 ? 0x00 : 0x01);
        if (!data) {
            LOGW("Failed to read pin %d of XL9535 multiplexer '%s': %s",
                pin, name.c_str(), data.error().message.c_str());
            return 0;
        }
        return (*data >> (pin % 8)) & 1;
    }

private:
    Xl9535(const std::string& name, const std::shared_ptr<I2CDevice>& device)
        : Peripheral(name)
        , device(device) {
    }

    void writeRegister(uint8_t reg, uint8_t value) {
        auto written = device->writeRegByte(reg, value);
        if (!written) {
            LOGW("Failed to write register 0x%02x of XL9535 multiplexer '%s': %s",
                reg, name.c_str(), written.error().message.c_str());
        }
    }

    static constexpr uint16_t bit(uint8_t pin) {
        return static_cast<uint16_t>(1U << pin);
    }

    static constexpr uint8_t lowByte(uint16_t value) {
        return static_cast<uint8_t>(value & 0xFFU);
    }

    static constexpr uint8_t highByte(uint16_t value) {
        return static_cast<uint8_t>(value >> 8U);
    }

    void updateDirection1() {
        writeRegister(0x06, lowByte(direction));
    }

    void updateDirection2() {
        writeRegister(0x07, highByte(direction));
    }

    void updateOutput1() {
        writeRegister(0x02, lowByte(output));
    }

    void updateOutput2() {
        writeRegister(0x03, highByte(output));
    }

    std::shared_ptr<I2CDevice> device;

    uint16_t direction = 0xFFFF;
    uint16_t output = 0;
};

class Xl9535Pin final : public Pin {
public:
    Xl9535Pin(const std::string& name, const std::shared_ptr<Xl9535>& mpx, uint8_t pin)
        : Pin(name)
        , mpx(mpx)
        , pin(pin) {
    }

    void pinMode(Mode mode) const override {
        mpx->pinMode(pin, mode);
    }

    void digitalWrite(uint8_t val) const override {
        mpx->digitalWrite(pin, val);
    }

    int digitalRead() const override {
        return mpx->digitalRead(pin);
    }

private:
    std::shared_ptr<Xl9535> mpx;
    const uint8_t pin;
};

inline PeripheralFactory makeFactoryForXl9535() {
    return makePeripheralFactory<Xl9535, Xl9535Settings>(
        "multiplexer:xl9535",
        "multiplexer",
        [](PeripheralInitParameters& params, const std::shared_ptr<Xl9535Settings>& settings) -> Result<std::shared_ptr<Xl9535>> {
            auto multiplexer = TRY(Xl9535::create(params.name, params.services.i2c, settings->parse()));

            // Register external pins
            for (int i = 0; i < 16; i++) {
                std::string pinName = params.name + ":" + std::to_string(i);
                LOGV("Registering external pin %s", pinName.c_str());
                auto pin = std::make_shared<Xl9535Pin>(pinName, multiplexer, i);
                Pin::registerPin(pinName, pin);
            }
            return multiplexer;
        });
}

}    // namespace cornucopia::ugly_duckling::peripherals::multiplexer
