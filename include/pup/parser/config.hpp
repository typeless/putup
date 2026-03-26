// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "eval.hpp"
#include "pup/core/result.hpp"

#include <string>
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
auto parse_config(std::string_view path, StringPool& pool) -> Result<VarDb>;

/// Parse config from string content (for testing)
[[nodiscard]]
auto parse_config_string(std::string_view content, StringPool& pool) -> Result<VarDb>;

} // namespace pup::parser
