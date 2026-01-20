// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/env.hpp"

#include <cstdlib>

namespace pup::platform {

auto set_env(std::string const& name, std::string const& value) -> void
{
    setenv(name.c_str(), value.c_str(), 1);
}

auto unset_env(std::string const& name) -> void
{
    unsetenv(name.c_str());
}

} // namespace pup::platform
