// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "pup/core/result.hpp"

#include <filesystem>
#include <optional>

namespace pup {

/// Encapsulates source/output directory layout for a build.
/// Enables read-only source directories by separating source and output trees.
struct ProjectLayout {
    std::filesystem::path source_root; ///< Where Tupfile.ini lives (may be read-only)
    std::filesystem::path output_root; ///< Where tup.config/.pup lives (writable)
    std::filesystem::path variant_dir; ///< Variant subdirectory within output_root

    /// True if source and output are the same (in-tree build)
    [[nodiscard]] auto is_in_tree() const -> bool
    {
        return source_root == output_root;
    }

    /// Get path to .pup directory
    [[nodiscard]] auto pup_dir() const -> std::filesystem::path
    {
        if (variant_dir.empty())
            return output_root / ".pup";
        return output_root / variant_dir / ".pup";
    }

    /// Get path to index file
    [[nodiscard]] auto index_path() const -> std::filesystem::path
    {
        return pup_dir() / "index";
    }

    /// Resolve a source-relative path to absolute
    [[nodiscard]] auto resolve_source(std::filesystem::path const& rel) const
        -> std::filesystem::path
    {
        return source_root / rel;
    }

    /// Resolve an output-relative path to absolute
    [[nodiscard]] auto resolve_output(std::filesystem::path const& rel) const
        -> std::filesystem::path
    {
        if (variant_dir.empty())
            return output_root / rel;
        return output_root / variant_dir / rel;
    }
};

/// Options for layout discovery
struct LayoutOptions {
    std::optional<std::filesystem::path> source_dir; ///< -S argument
    std::optional<std::filesystem::path> build_dir;  ///< -B argument
};

/// Discover project layout from options and environment.
/// Priority: CLI args > environment variables > cwd detection
[[nodiscard]] auto discover_layout(LayoutOptions const& opts = {}) -> Result<ProjectLayout>;

/// Find project root by walking up from start_dir looking for Tupfile.ini
[[nodiscard]] auto find_project_root(std::filesystem::path const& start_dir)
    -> std::optional<std::filesystem::path>;

/// Find variant directory containing tup.config
[[nodiscard]] auto find_variant_dir(std::filesystem::path const& root)
    -> std::optional<std::filesystem::path>;

} // namespace pup
