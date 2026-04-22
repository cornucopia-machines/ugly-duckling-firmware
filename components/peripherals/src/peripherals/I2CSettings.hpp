#pragma once

#include <chrono>

#include <Configuration.hpp>
#include <I2CManager.hpp>

using namespace std::chrono;
using namespace cornucopia::ugly_duckling::kernel;

namespace cornucopia::ugly_duckling::peripherals {

class I2CSettings
    : public ConfigurationSection {
public:
    // I2C address is typically a hexadecimal number,
    // but JSON doesn't support 0x notation, so we
    // take it as a string instead
    Property<std::string> address { this, "address" };
    Property<InternalPinPtr> sda { this, "sda" };
    Property<InternalPinPtr> scl { this, "scl" };
    // Clock speed in kHz (e.g. 100 for Standard Mode, 400 for Fast Mode)
    Property<uint32_t> clkSpeed { this, "clkSpeed", 400 };

    I2CConfig parse(uint8_t defaultAddress = 0xFF, const InternalPinPtr& defaultSda = nullptr, const InternalPinPtr& defaultScl = nullptr) const {
        return {
            .address = address.get().empty()
                ? defaultAddress
                : static_cast<uint8_t>(strtol(address.get().c_str(), nullptr, 0)),
            .sda = sda.get() == nullptr
                ? defaultSda
                : sda.get(),
            .scl = scl.get() == nullptr
                ? defaultScl
                : scl.get(),
            .clkSpeed = clkSpeed.get() * 1000
        };
    }
};

}    // namespace cornucopia::ugly_duckling::peripherals
