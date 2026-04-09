#pragma once

#include <string>

namespace cornucopia::ugly_duckling::peripherals::api {

struct IPeripheral {
    virtual ~IPeripheral() = default;

    virtual const std::string& getName() const = 0;
};

} // namespace cornucopia::ugly_duckling::peripherals::api
