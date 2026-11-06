#pragma once

/*
 * ISc16is752Bus on an ESP-IDF I2CDevice. Kept apart from Sc16is752.hpp, which is pure C++ and
 * compiles in native unit tests.
 */

#include "Sc16is752.hpp"
#include <I2CManager.hpp>
#include <Result.hpp>

#include <memory>
#include <utility>
#include <vector>

namespace cornucopia::ugly_duckling::peripherals::serial {

class Sc16is752I2CBus final : public ISc16is752Bus {
public:
    explicit Sc16is752I2CBus(std::shared_ptr<kernel::I2CDevice> device)
        : device(std::move(device)) {
    }

    Result<uint8_t> readRegister(uint8_t subaddress) override {
        return device->readRegByte(subaddress);
    }

    Result<> writeRegister(uint8_t subaddress, uint8_t value) override {
        return device->writeRegByte(subaddress, value);
    }

    Result<> readBurst(uint8_t subaddress, uint8_t* buffer, size_t length) override {
        return device->readReg(subaddress, buffer, length);
    }

    Result<> writeBurst(uint8_t subaddress, const uint8_t* buffer, size_t length) override {
        // I2CDevice::writeReg takes a mutable buffer
        std::vector<uint8_t> copy(buffer, buffer + length);
        return device->writeReg(subaddress, copy.data(), copy.size());
    }

private:
    const std::shared_ptr<kernel::I2CDevice> device;
};

}    // namespace cornucopia::ugly_duckling::peripherals::serial
