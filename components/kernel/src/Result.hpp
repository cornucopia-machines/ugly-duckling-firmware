#pragma once

#include <expected>
#include <string>
#include <utility>

namespace cornucopia::ugly_duckling::kernel {

/**
 * @brief Why an operation failed, as a human-readable message.
 *
 * @details The firmware is built without C++ exceptions; fallible operations return a Result
 * instead. The message ends up in logs and in the init message sent to the server, so it should
 * say what failed in terms someone looking at the device's configuration can act on.
 */
struct Error {
    std::string message;
};

template <typename T = void>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> failure(std::string message) {
    return std::unexpected(Error { std::move(message) });
}

}    // namespace cornucopia::ugly_duckling::kernel

/**
 * @brief Evaluate to the value of `expr` (a Result), or return its error from the enclosing function.
 * Usage: `auto device = TRY(i2c->createDevice(name, config));` or `TRY(device->writeRegByte(reg, value));`
 *
 * @details Uses a GNU statement expression (supported by GCC and Clang) so that it can be used as an
 * expression. Inside a lambda, the error is returned from the lambda.
 */
#define TRY(expr)                                                \
    ({                                                           \
        auto _result_ = (expr);                                  \
        if (!_result_.has_value()) {                             \
            return std::unexpected(std::move(_result_).error()); \
        }                                                        \
        *std::move(_result_);                                    \
    })
