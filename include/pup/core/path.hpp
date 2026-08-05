// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"

#include <string_view>

namespace pup {

/// Path string operations. All functions operate on forward-slash-separated
/// UTF-8 paths without touching the filesystem.
namespace path {

/// Join two path segments with '/'.
/// join("src", "foo.c") -> "src/foo.c"
/// join("src/", "foo.c") -> "src/foo.c"
/// join("", "foo.c") -> "foo.c"
/// join("src", "/usr/include") -> "/usr/include" (absolute rhs replaces)
[[nodiscard]]
auto join(std::string_view a, std::string_view b) -> StringId;

/// Get the parent directory.
/// parent("src/lib/foo.c") -> "src/lib"
/// parent("foo.c") -> ""
/// parent("") -> ""
/// parent("/") -> "/"
[[nodiscard]]
auto parent(std::string_view p) -> std::string_view;

/// Get the filename component (after last '/').
[[nodiscard]]
auto filename(std::string_view p) -> std::string_view;

/// Get the stem (filename without extension).
[[nodiscard]]
auto stem(std::string_view p) -> std::string_view;

/// Get the file extension (including dot).
[[nodiscard]]
auto extension(std::string_view p) -> std::string_view;

/// Check if a path is absolute.
[[nodiscard]]
auto is_absolute(std::string_view p) -> bool;

/// Check if a path is exactly its own root ("/" or, on Windows, "C:/").
/// Upward walks stop here: parent() of a root is the root itself.
[[nodiscard]]
auto is_root(std::string_view p) -> bool;

/// Lexically normalize a path by resolving '.' and '..' segments.
/// Does not touch the filesystem.
[[nodiscard]]
auto normalize(std::string_view p) -> StringId;

/// Compute relative path from base to target (lexical, no filesystem access).
[[nodiscard]]
auto relative(std::string_view target, std::string_view base) -> StringId;

} // namespace path
} // namespace pup
