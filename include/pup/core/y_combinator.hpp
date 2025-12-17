// Y-combinator for recursive lambdas in C++20
// Enables recursive lambdas without std::function overhead
#pragma once

namespace pup {

template<typename F>
struct YCombinator {
    F f;

    template<typename... Args>
    constexpr auto operator()(Args&&... args) const
        -> decltype(f(*this, std::forward<Args>(args)...))
    {
        return f(*this, std::forward<Args>(args)...);
    }
};

template<typename F>
YCombinator(F) -> YCombinator<F>;

} // namespace pup
