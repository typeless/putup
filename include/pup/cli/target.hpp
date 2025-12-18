// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "pup/core/expected.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pup {

/// Parsed target specifying variant, scope, and whether it's an output file
struct Target {
    std::optional<std::filesystem::path> variant;
    std::filesystem::path scope_or_output;
    bool is_output = false;
};

/// Parse a single target path into its variant and scope/output components
///
/// Detection logic:
/// 1. If first component contains tup.config, it's a variant
/// 2. Remainder is classified as directory (scope) or file (output)
/// 3. Source files (*.c, *.cpp, etc.) return error
///
/// @param project_root The project root directory
/// @param target_path The target path to parse (relative to project_root)
/// @return Parsed Target or error message
[[nodiscard]] auto parse_target(
    std::filesystem::path const& project_root,
    std::string const& target_path
) -> expected<Target, std::string>;

/// Expand a glob pattern into multiple targets
///
/// Patterns like "build-*" expand to all matching variants.
/// Patterns like "build-*/src/lib" expand to variant + scope pairs.
///
/// @param project_root The project root directory
/// @param pattern Glob pattern (e.g., "build-*", "build-*/src/lib")
/// @return Vector of expanded targets
[[nodiscard]] auto expand_glob_target(
    std::filesystem::path const& project_root,
    std::string const& pattern
) -> std::vector<Target>;

/// Validate that multiple targets are consistent
///
/// All targets must be the same type:
/// - All have explicit variant, OR
/// - All have no variant (applies to all variants)
///
/// @param project_root The project root directory
/// @param targets Target paths to validate
/// @return Vector of parsed targets, or error message
[[nodiscard]] auto validate_target_consistency(
    std::filesystem::path const& project_root,
    std::vector<std::string> const& targets
) -> expected<std::vector<Target>, std::string>;

} // namespace pup
