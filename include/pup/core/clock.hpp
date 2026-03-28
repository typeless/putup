// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <chrono>
#include <cstdint>

namespace pup {

/// Monotonic clock for measuring durations and timeouts.
/// Replaces std::chrono::steady_clock with a platform-implemented now().
struct SteadyClock {
    using duration = std::chrono::nanoseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<SteadyClock>;
    static constexpr bool is_steady = true;

    [[nodiscard]]
    static auto now() noexcept -> time_point;
};

/// Wall-clock time for timestamps.
/// Replaces std::chrono::system_clock with a platform-implemented now().
struct SystemClock {
    using duration = std::chrono::nanoseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<SystemClock>;
    static constexpr bool is_steady = false;

    [[nodiscard]]
    static auto now() noexcept -> time_point;
};

} // namespace pup
