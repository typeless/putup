// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/result.hpp"
#include "pup/core/string.hpp"
#include "pup/core/vec.hpp"

#include <string_view>

namespace pup::parser {

/// Options for glob expansion
struct GlobOptions {
    bool include_hidden = false; ///< Include files starting with '.'
    bool recursive = true;       ///< Expand ** patterns recursively
};

/// Result of glob expansion
struct GlobResult {
    Vec<String> matches;
    Vec<String> exclusions; ///< Files explicitly excluded with !
};

/// Glob pattern matcher
class Glob {
public:
    explicit Glob(std::string_view pattern);

    /// Check if a filename matches the pattern
    [[nodiscard]]
    auto matches(std::string_view filename) const -> bool;

    /// Check if this is a simple pattern (no wildcards)
    [[nodiscard]]
    auto is_literal() const -> bool
    {
        return !has_wildcards_;
    }

    /// Get the literal value if no wildcards
    [[nodiscard]]
    auto literal() const -> std::string_view
    {
        return pattern_;
    }

    /// Check if pattern has ** (recursive)
    [[nodiscard]]
    auto is_recursive() const -> bool
    {
        return has_double_star_;
    }

    /// Get the pattern string
    [[nodiscard]]
    auto pattern() const -> std::string_view
    {
        return pattern_;
    }

private:
    String pattern_;
    bool has_wildcards_ = false;
    bool has_double_star_ = false;

    [[nodiscard]]
    auto match_recursive(std::string_view pattern, std::string_view text) const -> bool;
    [[nodiscard]]
    auto match_bracket(std::string_view& pattern, char c) const -> bool;
};

/// Expand a glob pattern to matching files
[[nodiscard]]
auto glob_expand(
    std::string_view pattern,
    std::string_view base_dir,
    GlobOptions const& options = {}
) -> Result<Vec<String>>;

/// Expand multiple patterns, handling exclusions (patterns starting with !)
[[nodiscard]]
auto glob_expand_all(
    Vec<String> const& patterns,
    std::string_view base_dir,
    GlobOptions const& options = {}
) -> Result<GlobResult>;

/// Parse a path pattern into directory prefix and glob suffix
/// e.g., "src/foo/*.c" -> ("src/foo", "*.c")
[[nodiscard]]
auto glob_split_path(
    std::string_view pattern
) -> std::pair<std::string_view, std::string_view>;

/// Check if a pattern contains glob wildcards
[[nodiscard]]
auto has_glob_chars(std::string_view pattern) -> bool;

/// Extract the basename from a path
[[nodiscard]]
auto path_basename(std::string_view path) -> std::string_view;

/// Extract the basename without extension
[[nodiscard]]
auto path_stem(std::string_view path) -> std::string_view;

/// Extract the extension from a path
[[nodiscard]]
auto path_extension(std::string_view path) -> std::string_view;

/// Extract the directory from a path
[[nodiscard]]
auto path_directory(std::string_view path) -> std::string_view;

/// Extract the portion of filename that matched the * in a glob pattern.
/// Returns empty string if pattern doesn't match or has no wildcard.
/// Example: pattern="*_test.c", filename="foo_test.c" -> "foo"
/// Example: pattern="*.c", filename="hello.c" -> "hello"
/// For path patterns, matches against basename only.
[[nodiscard]]
auto glob_match_extract(std::string_view pattern, std::string_view filename) -> String;

} // namespace pup::parser
