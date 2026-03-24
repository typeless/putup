// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/env.hpp"

#include <cstdlib>

namespace pup::platform {

auto set_env(std::string_view name, std::string_view value) -> void
{
    // data() is null-terminated: all callers provide pup::String, std::string, or literals
    _putenv_s(name.data(), value.data());
}

auto unset_env(std::string_view name) -> void
{
    _putenv_s(name.data(), "");
}

} // namespace pup::platform
