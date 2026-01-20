// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <string>

namespace pup::platform {

auto set_env(std::string const& name, std::string const& value) -> void;
auto unset_env(std::string const& name) -> void;

} // namespace pup::platform
