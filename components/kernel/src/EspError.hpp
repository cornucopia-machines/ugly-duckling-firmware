#pragma once

#include <Result.hpp>

#include <esp_err.h>

#include <string>

namespace cornucopia::ugly_duckling::kernel {

inline Error espError(esp_err_t err) {
    return Error { esp_err_to_name(err) };
}

inline Result<> espResult(esp_err_t err) {
    if (err != ESP_OK) {
        return std::unexpected(espError(err));
    }
    return {};
}

}    // namespace cornucopia::ugly_duckling::kernel

/**
 * @brief Return the esp_err_t as an Error from the enclosing function (which must return a Result)
 * if it is not ESP_OK.
 *
 * Use ESP_ERROR_CHECK instead where a failure means the firmware itself is broken, e.g. a
 * peripheral driver that cannot be installed on a pin the board definition hard-codes.
 */
#define ESP_TRY(err)                                                                      \
    do {                                                                                  \
        esp_err_t _err_ = (err);                                                          \
        if (_err_ != ESP_OK) {                                                            \
            return std::unexpected(::cornucopia::ugly_duckling::kernel::espError(_err_)); \
        }                                                                                 \
    } while (0)
