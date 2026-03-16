// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pup::cli {

/// Output mode for file operations
struct OutputMode {
    bool dry_run = false;
    bool verbose = false;
};

/// Result of file removal operations
struct RemoveResult {
    std::size_t removed_count = 0;
    std::size_t error_count = 0;
    std::vector<std::string> output_dirs = {};
};

/// Remove empty directories from deepest to shallowest
/// Returns count of directories removed
auto remove_empty_directories(
    std::vector<std::string> const& dirs,
    std::string const& build_dir,
    std::string const& source_dir,
    OutputMode mode
) -> std::size_t;

/// Escape string for DOT format (graphviz)
[[nodiscard]]
auto escape_dot_label(std::string_view s) -> std::string;

/// Escape string for JSON (RFC 8259)
[[nodiscard]]
auto escape_json(std::string_view s) -> std::string;

} // namespace pup::cli
