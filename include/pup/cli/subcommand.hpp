// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"

#include <string_view>

namespace pup::cli {

/// Resolve the external subcommand `name` to the `putup-<name>` executable held
/// by the first entry of `search_path`, a PATH-formatted directory list. Empty
/// when no entry holds it, or when `name` is not a bare executable name.
[[nodiscard]]
auto find_subcommand(std::string_view name, std::string_view search_path) -> StringId;

} // namespace pup::cli
