// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <cstddef>
#include <string_view>

namespace pup::cli {

/// Output mode for file operations
struct OutputMode {
    bool dry_run = false;
    bool verbose = false;
};

/// Outcome of a removal pass
struct RemoveResult {
    std::size_t removed_count = 0;
    std::size_t error_count = 0;
};

/// Remove empty directories from deepest to shallowest
auto remove_empty_directories(
    Vec<StringId> const& dirs,
    std::string_view build_dir,
    std::string_view source_dir,
    OutputMode mode
) -> RemoveResult;

/// Escape string for DOT format (graphviz)
[[nodiscard]]
auto escape_dot_label(std::string_view s) -> StringId;

/// Escape string for JSON (RFC 8259)
[[nodiscard]]
auto escape_json(std::string_view s) -> StringId;

} // namespace pup::cli
