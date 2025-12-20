// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "options.hpp"

namespace pup::cli {

/// Parse and validate Tupfiles
[[nodiscard]]
auto cmd_parse(Options const& opts) -> int;

/// Execute build
[[nodiscard]]
auto cmd_build(Options const& opts) -> int;

/// Export build info (script, compdb, graph)
[[nodiscard]]
auto cmd_export(Options const& opts) -> int;

/// Remove generated files
[[nodiscard]]
auto cmd_clean(Options const& opts) -> int;

/// Full reset: remove .pup and variant directory
[[nodiscard]]
auto cmd_distclean(Options const& opts) -> int;

/// Execute config-generating rules (outputs tup.config files)
[[nodiscard]]
auto cmd_configure(Options const& opts) -> int;

/// Dispatch command based on opts.command
[[nodiscard]]
auto dispatch(Options const& opts) -> int;

} // namespace pup::cli
