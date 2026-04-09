#pragma once

#include <chrono>
#include <functional>
#include <optional>

#include <Concurrent.hpp>

using namespace std::chrono;
using namespace std::chrono_literals;
using namespace cornucopia::ugly_duckling::kernel;

namespace cornucopia::ugly_duckling::utils {

template <typename T>
struct DebouncedParams {
    T& lastValue;
    std::optional<std::chrono::steady_clock::time_point> lastMeasurement;
};

template <typename T>
class DebouncedMeasurement {
public:
    DebouncedMeasurement(
        std::move_only_function<std::optional<T>(const DebouncedParams<T>)> measure,
        std::chrono::milliseconds interval = 1s,
        const T& defaultValue = {})
        : measure(std::move(measure))
        , interval(interval)
        , value(defaultValue) {
    }

    void updateIfNecessary() {
        Lock lock(mutex);
        auto now = steady_clock::now();
        if (lastMeasurement && now - *lastMeasurement < interval) {
            return;
        }
        auto measurement = measure({
            .lastValue = value,
            .lastMeasurement = lastMeasurement,
        });
        if (measurement) {
            value = *measurement;
            lastMeasurement = now;
        }
    }

    T getValue() {
        updateIfNecessary();
        return value;
    }

private:
    std::move_only_function<std::optional<T>(DebouncedParams<T>)> measure;
    std::chrono::milliseconds interval;

    T value;
    std::optional<std::chrono::steady_clock::time_point> lastMeasurement;

    Mutex mutex;
};

}    // namespace cornucopia::ugly_duckling::utils
