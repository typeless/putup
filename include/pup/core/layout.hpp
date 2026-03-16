// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/path.hpp"
#include "pup/core/result.hpp"

#include <optional>
#include <string>
#include <vector>

namespace pup {

struct ProjectLayout {
    std::string source_root;
    std::string config_root;
    std::string output_root;

    [[nodiscard]]
    auto is_in_tree() const -> bool
    {
        return source_root == output_root;
    }

    [[nodiscard]]
    auto has_separate_config() const -> bool
    {
        return config_root != source_root;
    }

    [[nodiscard]]
    auto pup_dir() const -> std::string
    {
        return path::join(output_root, ".pup");
    }

    [[nodiscard]]
    auto index_path() const -> std::string
    {
        return path::join(pup_dir(), "index");
    }

    [[nodiscard]]
    auto resolve_source(std::string const& rel) const -> std::string
    {
        return path::join(source_root, rel);
    }

    [[nodiscard]]
    auto resolve_config(std::string const& rel) const -> std::string
    {
        return path::join(config_root, rel);
    }

    [[nodiscard]]
    auto resolve_output(std::string const& rel) const -> std::string
    {
        return path::join(output_root, rel);
    }
};

struct LayoutOptions {
    std::optional<std::string> source_dir;
    std::optional<std::string> config_dir;
    std::optional<std::string> build_dir;
};

[[nodiscard]]
auto discover_layout(LayoutOptions const& opts = {}) -> Result<ProjectLayout>;

[[nodiscard]]
auto find_project_root(
    std::string const& start_dir
) -> std::optional<std::string>;

[[nodiscard]]
auto discover_variants(
    std::string const& source_root
) -> std::vector<std::string>;

} // namespace pup
