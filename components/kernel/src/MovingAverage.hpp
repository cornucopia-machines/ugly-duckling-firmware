#pragma once

#include <vector>

namespace cornucopia::ugly_duckling::kernel {

template <typename M, typename T = M>
requires std::is_arithmetic_v<M> && std::is_arithmetic_v<T>
class MovingAverage {
public:
    MovingAverage(std::size_t maxMeasurements)
        : maxMeasurements(maxMeasurements)
        , measurements(maxMeasurements, M(0))
        , sum(0)
        , average(0) {
    }

    void record(M measurement) {
        if (count == maxMeasurements) {
            sum -= measurements[currentIndex];
        } else {
            ++count;
        }

        measurements[currentIndex] = measurement;
        sum += measurement;
        average = sum / count;

        currentIndex = (currentIndex + 1) % maxMeasurements;
    }

    constexpr T getAverage() const {
        return average;
    }

private:
    const std::size_t maxMeasurements;
    std::vector<M> measurements;
    std::size_t currentIndex{0};
    std::size_t count{0};
    T sum;
    T average;
};

}    // namespace cornucopia::ugly_duckling::kernel
