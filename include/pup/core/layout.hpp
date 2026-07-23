// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <optional>
#include <string_view>

namespace pup {

struct ProjectLayout {
    StringId source_root = StringId::Empty;
    StringId config_root = StringId::Empty;
    StringId output_root = StringId::Empty;

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
    auto pup_dir() const -> StringId;

    [[nodiscard]]
    auto index_path() const -> StringId;

    [[nodiscard]]
    auto resolve_source(std::string_view rel) const -> StringId;

    [[nodiscard]]
    auto resolve_config(std::string_view rel) const -> StringId;

    [[nodiscard]]
    auto resolve_output(std::string_view rel) const -> StringId;
};

struct LayoutOptions {
    std::optional<StringId> source_dir;
    std::optional<StringId> config_dir;
    std::optional<StringId> build_dir;
};

[[nodiscard]]
auto discover_layout(LayoutOptions const& opts = {}) -> Result<ProjectLayout>;

[[nodiscard]]
auto find_project_root(
    std::string_view start_dir
) -> std::optional<StringId>;

[[nodiscard]]
auto discover_variants(
    std::string_view source_root,
    std::string_view project_root
) -> Vec<StringId>;

[[nodiscard]]
auto find_build_subdir(
    std::string_view root
) -> std::optional<StringId>;

auto record_build_dir_owner(ProjectLayout const& layout) -> void;

[[nodiscard]]
auto foreign_build_dir_owner(
    std::string_view build_dir,
    std::string_view project_root
) -> std::optional<StringId>;

} // namespace pup
