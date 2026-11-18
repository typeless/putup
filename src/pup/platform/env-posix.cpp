// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/clock.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/platform.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/env.hpp"
#include "pup/platform/sys.hpp"

#include <cstdlib>

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

auto get_env(char const* name) -> char const*
{
    return platform::sys::getenv(name);
}

} // namespace pup::platform

namespace pup {

auto get_platform() -> StringId
{
    if (auto const* env = platform::sys::getenv("TUP_PLATFORM"); env && *env) {
        return global_pool().intern(env);
    }
    return global_pool().intern(PLATFORM);
}

auto cpu_count() -> std::size_t
{
    return static_cast<std::size_t>(platform::sys::cpu_count());
}

auto SteadyClock::now() noexcept -> time_point
{
    return time_point { duration { platform::sys::clock_monotonic_ns() } };
}

auto SystemClock::now() noexcept -> time_point
{
    return time_point { duration { platform::sys::clock_realtime_ns() } };
}

} // namespace pup
