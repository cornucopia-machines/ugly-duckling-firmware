#pragma once

#include <chrono>

using namespace std::chrono_literals;

namespace cornucopia::ugly_duckling::kernel {

using ticks = std::chrono::duration<uint32_t, std::ratio<1, configTICK_RATE_HZ>>;

inline static ticks clampTicks(std::chrono::milliseconds duration) {
    if (duration < 0ms) {
        return ticks::zero();
    }
    if (duration > ticks::max()) {
        return ticks::max();
    }
    return std::chrono::duration_cast<ticks>(duration);
}

} // namespace cornucopia::ugly_duckling::kernel
