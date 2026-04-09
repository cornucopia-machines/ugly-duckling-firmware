#pragma once

#include <stdexcept>

namespace cornucopia::ugly_duckling::peripherals {

class PeripheralCreationException
    : public std::runtime_error {
public:
    PeripheralCreationException(const std::string& reason)
        : std::runtime_error(reason) {
    }
};

}    // namespace cornucopia::ugly_duckling::peripherals
