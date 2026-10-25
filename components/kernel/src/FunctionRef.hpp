#pragma once

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace cornucopia::ugly_duckling::kernel {

template <typename Signature>
class FunctionRef;

/**
 * @brief Non-owning reference to a callable, for callbacks that are only called before the function
 * taking them returns.
 *
 * @details Unlike std::function, it never allocates and needs no per-callable code to copy and destroy
 * the callable, which costs a few hundred bytes of flash for every lambda passed as a std::function.
 * It refers to the callable instead of copying it, so the callable must outlive the FunctionRef:
 * passing a temporary lambda as an argument is fine, storing a FunctionRef beyond the call is not.
 *
 * Mirrors C++26 std::function_ref; replace it with that once our toolchain's libstdc++ provides it
 * (GCC 16).
 */
template <typename R, typename... Args>
class FunctionRef<R(Args...)> {
public:
    template <typename F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, FunctionRef> && std::is_invocable_r_v<R, F&, Args...>)
    // Constrained above, so it doesn't hijack copying; and `target` is referred to, not forwarded
    FunctionRef(F&& target) noexcept                                                       // NOLINT(bugprone-forwarding-reference-overload,cppcoreguidelines-missing-std-forward)
        : callable(const_cast<void*>(static_cast<const void*>(std::addressof(target))))    // NOLINT(cppcoreguidelines-pro-type-const-cast)
        , invoker([](void* erased, Args... args) -> R {
            if constexpr (std::is_void_v<R>) {
                std::invoke(*static_cast<std::remove_reference_t<F>*>(erased), std::forward<Args>(args)...);
            } else {
                return std::invoke(*static_cast<std::remove_reference_t<F>*>(erased), std::forward<Args>(args)...);
            }
        }) {
    }

    R operator()(Args... args) const {
        return invoker(callable, std::forward<Args>(args)...);
    }

private:
    void* callable;
    R (*invoker)(void*, Args...);
};

}    // namespace cornucopia::ugly_duckling::kernel
