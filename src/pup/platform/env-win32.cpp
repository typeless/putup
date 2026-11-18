// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/clock.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/platform.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/env.hpp"

#include <cstdlib>
#include <windows.h>

namespace pup::platform {

auto set_env(std::string_view name, std::string_view value) -> void
{
    // data() is null-terminated: all callers provide pool string_views, Buf, or literals
    _putenv_s(name.data(), value.data());
}

auto unset_env(std::string_view name) -> void
{
    _putenv_s(name.data(), "");
}

auto get_env(char const* name) -> char const*
{
    return std::getenv(name);
}

} // namespace pup::platform

namespace pup {

auto get_platform() -> StringId
{
    if (auto const* env = std::getenv("TUP_PLATFORM"); env && *env) {
        return global_pool().intern(env);
    }
    return global_pool().intern(PLATFORM);
}

auto cpu_count() -> std::size_t
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? si.dwNumberOfProcessors : 1;
}

auto SteadyClock::now() noexcept -> time_point
{
    static auto freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    // Split to avoid int64 overflow (counter * 1e9 overflows after ~2.5h at 10MHz)
    auto whole_seconds = counter.QuadPart / freq;
    auto remainder = counter.QuadPart % freq;
    auto ns = whole_seconds * 1'000'000'000 + remainder * 1'000'000'000 / freq;
    return time_point { duration { ns } };
}

auto SystemClock::now() noexcept -> time_point
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    auto ticks = (static_cast<std::int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    // FILETIME is 100ns intervals since 1601-01-01. Convert to Unix epoch ns.
    auto constexpr EPOCH_DIFF = std::int64_t { 116444736000000000 };
    auto ns = (ticks - EPOCH_DIFF) * 100;
    return time_point { duration { ns } };
}

} // namespace pup
