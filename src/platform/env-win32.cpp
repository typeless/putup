// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/platform/env.hpp"

#include <cstdlib>

namespace pup::platform {

auto set_env(std::string const& name, std::string const& value) -> void
{
    _putenv_s(name.c_str(), value.c_str());
}

auto unset_env(std::string const& name) -> void
{
    _putenv_s(name.c_str(), "");
}

} // namespace pup::platform
