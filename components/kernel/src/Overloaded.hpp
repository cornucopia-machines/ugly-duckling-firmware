#pragma once

namespace cornucopia::ugly_duckling::kernel {

template <typename... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

}    // namespace cornucopia::ugly_duckling::kernel
