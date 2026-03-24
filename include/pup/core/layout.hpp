// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/path.hpp"
#include "pup/core/result.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pup {

struct ProjectLayout {
    String source_root;
    String config_root;
    String output_root;

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
    auto pup_dir() const -> String
    {
        return path::join(output_root, ".pup");
    }

    [[nodiscard]]
    auto index_path() const -> String
    {
        return path::join(pup_dir(), "index");
    }

    [[nodiscard]]
    auto resolve_source(std::string_view rel) const -> String
    {
        return path::join(source_root, rel);
    }

    [[nodiscard]]
    auto resolve_config(std::string_view rel) const -> String
    {
        return path::join(config_root, rel);
    }

    [[nodiscard]]
    auto resolve_output(std::string_view rel) const -> String
    {
        return path::join(output_root, rel);
    }
};

struct LayoutOptions {
    std::optional<String> source_dir;
    std::optional<String> config_dir;
    std::optional<String> build_dir;
};

[[nodiscard]]
auto discover_layout(LayoutOptions const& opts = {}) -> Result<ProjectLayout>;

[[nodiscard]]
auto find_project_root(
    std::string_view start_dir
) -> std::optional<String>;

[[nodiscard]]
auto discover_variants(
    std::string_view source_root
) -> std::vector<std::string>;

} // namespace pup
