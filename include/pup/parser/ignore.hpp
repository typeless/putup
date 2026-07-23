// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <optional>
#include <string_view>

namespace pup::parser {

/// Gitignore-style directory patterns: built-in discovery defaults and -x excludes.
/// All callers filter directories, so a trailing '/' is accepted and stripped.
class IgnoreList final {
public:
    IgnoreList() = default;

    /// Create an IgnoreList with default patterns (.git/, .pup/, node_modules/)
    static auto with_defaults() -> IgnoreList;

    /// Add a pattern string (gitignore glob syntax; negation is not supported,
    /// a '!' pattern is rejected)
    auto add(std::string_view pattern) -> void;

    /// Check if a relative path should be ignored
    [[nodiscard]]
    auto is_ignored(std::string_view rel_path) const -> bool;

    /// Check if a directory or any of its ancestors is ignored.
    /// Flat-list equivalent of pruning the walk at an ignored directory.
    [[nodiscard]]
    auto is_ignored_dir(std::string_view rel_path) const -> bool;

    /// Check if the list has no patterns
    [[nodiscard]]
    auto empty() const -> bool
    {
        return patterns_.empty();
    }

private:
    pup::Vec<StringId> patterns_;

    /// Parse a single pattern line (trailing '/' stripped, '!' rejected)
    static auto parse_pattern(std::string_view line) -> std::optional<StringId>;

    /// Check if a path matches a pattern; a pattern containing '/' is anchored
    /// (matched against the full path), otherwise it matches the basename
    [[nodiscard]]
    static auto match_pattern(std::string_view pattern, std::string_view path) -> bool;

    /// Match a glob pattern against a string
    [[nodiscard]]
    static auto glob_match(std::string_view pattern, std::string_view text) -> bool;

    /// Recursive glob matching helper
    [[nodiscard]]
    static auto glob_match_recursive(std::string_view pattern, std::string_view text) -> bool;
};

} // namespace pup::parser
