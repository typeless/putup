// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pup {

/// Check if path is under root directory (handles trailing separators).
/// Returns true if path == root or path is a descendant of root.
[[nodiscard]]
auto is_path_under(
    std::filesystem::path const& path,
    std::filesystem::path const& root
) -> bool;

/// Get relative path from root, or empty string if not under root.
[[nodiscard]]
auto relative_to_root(
    std::filesystem::path const& path,
    std::filesystem::path const& root
) -> std::string;

/// Check if string path is under scope prefix (directory-boundary aware).
/// Empty scope matches all paths.
[[nodiscard]]
auto is_path_in_scope(
    std::string_view path,
    std::string_view scope
) -> bool;

/// Check if path is under any of the given scopes.
/// Empty scopes vector matches all paths.
[[nodiscard]]
auto is_path_in_any_scope(
    std::string_view path,
    std::vector<std::string> const& scopes
) -> bool;

/// Compute the "../" prefix needed to navigate from source_dir back to project root.
/// E.g., "src/lib" → "../../", "" → ""
[[nodiscard]]
auto compute_source_to_root(std::string_view source_dir) -> std::string;

/// Strip a prefix from a path if present.
/// E.g., strip_path_prefix("build/src/foo.o", "build") → "src/foo.o"
[[nodiscard]]
auto strip_path_prefix(
    std::string_view path,
    std::string_view prefix
) -> std::string;

/// Check if path (when resolved from source_root) is under target_root.
/// If so, return the relative path within target_root.
/// E.g., resolve_under_root("../build/include/foo.h", "/src", "/src/../build")
///       → "include/foo.h"
[[nodiscard]]
auto resolve_under_root(
    std::string_view path,
    std::filesystem::path const& source_root,
    std::filesystem::path const& target_root
) -> std::optional<std::string>;

/// Transform a project-root-relative path to be relative to a Tupfile's source directory.
/// Used for command expansion where commands run from the Tupfile directory.
///
/// Examples (source_dir = "src/lib", source_to_root = "../../"):
///   "src/lib/foo.c" → "foo.c"       (local file, strip prefix)
///   "src/lib"       → "."           (exact match)
///   "include/bar.h" → "../../include/bar.h" (other dir, prepend source_to_root)
///   "../data.txt"   → "../../../data.txt"   (already relative, prepend source_to_root)
///   "/usr/include"  → "/usr/include"        (absolute, unchanged)
[[nodiscard]]
auto make_source_relative(
    std::string_view path,
    std::string_view source_to_root,
    std::string_view source_dir
) -> std::string;

} // namespace pup
