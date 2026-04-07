#pragma once

namespace farmhub::kernel {

template <typename... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

}    // namespace farmhub::kernel
