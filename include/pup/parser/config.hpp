// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "eval.hpp"
#include "pup/core/result.hpp"

#include <filesystem>
#include <string_view>

namespace pup::parser {

/// Parse a tup.config file into a VarDb
///
/// Format:
///   CONFIG_NAME=value
///   # comments start with #
///   CONFIG_EMPTY=
///
/// The CONFIG_ prefix is stripped when storing, so CONFIG_DEBUG=y
/// becomes accessible as @(DEBUG) in Tupfiles.
[[nodiscard]]
auto parse_config(std::filesystem::path const& path) -> Result<VarDb>;

/// Parse config from string content (for testing)
[[nodiscard]]
auto parse_config_string(std::string_view content) -> Result<VarDb>;

} // namespace pup::parser
