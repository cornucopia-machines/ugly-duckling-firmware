#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <optional>

namespace cornucopia::ugly_duckling::utils {

template <class Rep1, class Period1, class Rep2, class Period2>
double chrono_ratio(
    std::chrono::duration<Rep1, Period1> a,
    std::chrono::duration<Rep2, Period2> b) {
    return std::chrono::duration<double>(a).count() / std::chrono::duration<double>(b).count();
}

template <class Rep, class Period>
std::optional<std::chrono::duration<Rep, Period>> minDuration(
    std::optional<std::chrono::duration<Rep, Period>> a,
    std::optional<std::chrono::duration<Rep, Period>> b) {
    if (!a) {
        return b;
    }
    if (!b) {
        return a;
    }
    return std::min(*a, *b);
}

template <class Rep, class Period>
std::optional<std::chrono::duration<Rep, Period>> minDuration(
    std::optional<std::chrono::duration<Rep, Period>> a,
    std::chrono::duration<Rep, Period> b) {
    if (!a) {
        return b;
    }
    return std::min(*a, b);
}

template <class Rep, class Period>
std::optional<std::chrono::duration<Rep, Period>> maxDuration(
    std::optional<std::chrono::duration<Rep, Period>> a,
    std::optional<std::chrono::duration<Rep, Period>> b) {
    if (!a) {
        return b;
    }
    if (!b) {
        return a;
    }
    return std::max(*a, *b);
}

template <class Rep, class Period>
std::optional<std::chrono::duration<Rep, Period>> maxDuration(
    std::optional<std::chrono::duration<Rep, Period>> a,
    std::chrono::duration<Rep, Period> b) {
    if (!a) {
        return b;
    }
    return std::max(*a, b);
}

}    // namespace cornucopia::ugly_duckling::utils
