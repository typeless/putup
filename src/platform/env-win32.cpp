// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/platform.hpp"
#include "pup/platform/env.hpp"

#include <cstdlib>
#include <windows.h>

namespace pup::platform {

auto set_env(std::string_view name, std::string_view value) -> void
{
    // data() is null-terminated: all callers provide pool string_views, HeapBuf, or literals
    _putenv_s(name.data(), value.data());
}

auto unset_env(std::string_view name) -> void
{
    _putenv_s(name.data(), "");
}

} // namespace pup::platform

namespace pup {

auto cpu_count() -> std::size_t
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? si.dwNumberOfProcessors : 1;
}

} // namespace pup
