// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/clock.hpp"
#include "pup/core/platform.hpp"
#include "pup/platform/env.hpp"

#include <cstdlib>
#include <ctime>
#include <unistd.h>

namespace pup::platform {

auto set_env(std::string_view name, std::string_view value) -> void
{
    // data() is null-terminated: all callers provide pup::String, std::string, or literals
    setenv(name.data(), value.data(), 1);
}

auto unset_env(std::string_view name) -> void
{
    unsetenv(name.data());
}

} // namespace pup::platform

namespace pup {

auto cpu_count() -> std::size_t
{
    auto n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<std::size_t>(n) : 1;
}

auto SteadyClock::now() noexcept -> time_point
{
    struct timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    auto ns = static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
    return time_point { duration { ns } };
}

auto SystemClock::now() noexcept -> time_point
{
    struct timespec ts {};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    auto ns = static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
    return time_point { duration { ns } };
}

} // namespace pup
