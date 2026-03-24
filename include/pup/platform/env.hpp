// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <string_view>

namespace pup::platform {

auto set_env(std::string_view name, std::string_view value) -> void;
auto unset_env(std::string_view name) -> void;

} // namespace pup::platform
