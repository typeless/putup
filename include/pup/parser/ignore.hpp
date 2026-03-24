// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/result.hpp"
#include "pup/core/string.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pup::parser {

/// Gitignore-style pattern for filtering paths
struct IgnorePattern {
    String pattern;
    bool negated = false;  ///< Pattern starts with !
    bool dir_only = false; ///< Pattern ends with /
    bool anchored = false; ///< Pattern contains / (not at end)
};

/// List of ignore patterns for filtering paths during Tupfile discovery
class IgnoreList {
public:
    IgnoreList() = default;

    /// Load patterns from a .pupignore file
    static auto load(std::string const& path) -> Result<IgnoreList>;

    /// Create an IgnoreList with default patterns (.git/, .pup/, node_modules/)
    static auto with_defaults() -> IgnoreList;

    /// Add a pattern string (gitignore syntax)
    auto add(std::string_view pattern) -> void;

    /// Check if a relative path should be ignored
    [[nodiscard]]
    auto is_ignored(std::string const& rel_path) const -> bool;

    /// Check if the list has no patterns
    [[nodiscard]]
    auto empty() const -> bool
    {
        return patterns_.empty();
    }

    /// Get number of patterns
    [[nodiscard]]
    auto size() const -> std::size_t
    {
        return patterns_.size();
    }

private:
    std::vector<IgnorePattern> patterns_;

    /// Parse a single pattern line
    static auto parse_pattern(std::string_view line) -> std::optional<IgnorePattern>;

    /// Check if a path matches a pattern
    [[nodiscard]]
    auto match_pattern(IgnorePattern const& p, std::string const& path) const -> bool;

    /// Match a glob pattern against a string
    [[nodiscard]]
    static auto glob_match(std::string_view pattern, std::string_view text) -> bool;

    /// Recursive glob matching helper
    [[nodiscard]]
    static auto glob_match_recursive(std::string_view pattern, std::string_view text) -> bool;
};

} // namespace pup::parser
