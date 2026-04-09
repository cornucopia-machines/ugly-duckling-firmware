#pragma once

#include <Configuration.hpp>
#include <Pin.hpp>
#include <utility>

namespace cornucopia::ugly_duckling::peripherals::multiplexer {

class Xl9535Settings
    : public I2CSettings {
};

class Xl9535 final
    : public Peripheral {
public:
    Xl9535(
        const std::string& name,
        const std::shared_ptr<I2CManager>& i2c,
        const I2CConfig& config)
        : Peripheral(name)
        , device(i2c->createDevice(name, config)) {

        LOGI("Initializing XL9535 multiplexer '%s' with %s",
            name.c_str(), config.toString().c_str());
    }

    void pinMode(uint8_t pin, Pin::Mode mode) {
        // TODO Signal if pull-up or pull-down is requested that we cannot support it
        if (mode == Pin::Mode::Output) {
            direction &= ~(1 << pin);
        } else {
            direction |= 1 << pin;
        }
        if (pin < 8) {
            updateDirection1();
        } else {
            updateDirection2();
        }
    }

    void digitalWrite(uint8_t pin, uint8_t val) {
        if (val == 1) {
            output |= 1 << pin;
        } else {
            output &= ~(1 << pin);
        }
        if (pin < 8) {
            updateOutput1();
        } else {
            updateOutput2();
        }
    }

    int digitalRead(uint8_t pin) {
        uint8_t data = device->readRegByte(pin < 8 ? 0x00 : 0x01);
        return (data >> (pin % 8)) & 1;
    }

private:
    void updateDirection1() {
        device->writeRegByte(0x06, direction & 0xFF);
    }

    void updateDirection2() {
        device->writeRegByte(0x07, direction >> 8);
    }

    void updateOutput1() {
        device->writeRegByte(0x02, output & 0xFF);
    }

    void updateOutput2() {
        device->writeRegByte(0x03, output >> 8);
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
    return makePeripheralFactory<Xl9535, Xl9535, Xl9535Settings>(
        "multiplexer:xl9535",
        "multiplexer",
        [](PeripheralInitParameters& params, const std::shared_ptr<Xl9535Settings>& settings) {
            auto multiplexer = std::make_shared<Xl9535>(
                params.name,
                params.services.i2c,
                settings->parse());

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
