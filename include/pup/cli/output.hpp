// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

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
    std::set<std::filesystem::path> output_dirs = {};
};

/// Remove a file with dry-run/verbose support
/// Returns true if file was (or would be) removed
auto remove_file(
    std::filesystem::path const& path,
    OutputMode mode
) -> bool;

/// Remove a directory if empty with dry-run/verbose support
/// Returns true if directory was (or would be) removed
auto remove_empty_dir(
    std::filesystem::path const& dir,
    std::filesystem::path const& root_guard,
    OutputMode mode
) -> bool;

/// Remove empty directories from deepest to shallowest
/// Returns count of directories removed
auto remove_empty_directories(
    std::set<std::filesystem::path> const& dirs,
    std::filesystem::path const& build_dir,
    std::filesystem::path const& source_dir,
    OutputMode mode
) -> std::size_t;

/// Escape string for DOT format (graphviz)
[[nodiscard]] auto escape_dot_label(std::string_view s) -> std::string;

/// Escape string for JSON (RFC 8259)
[[nodiscard]] auto escape_json(std::string_view s) -> std::string;

} // namespace pup::cli
