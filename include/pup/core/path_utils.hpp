// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <optional>
#include <string_view>

namespace pup {

[[nodiscard]]
auto is_path_under(
    std::string_view path,
    std::string_view root
) -> bool;

[[nodiscard]]
auto relative_to_root(
    std::string_view path,
    std::string_view root
) -> StringId;

[[nodiscard]]
auto is_path_in_scope(
    std::string_view path,
    std::string_view scope
) -> bool;

[[nodiscard]]
auto is_path_in_any_scope(
    std::string_view path,
    Vec<StringId> const& scopes
) -> bool;

[[nodiscard]]
auto compute_source_to_root(std::string_view source_dir) -> StringId;

[[nodiscard]]
auto strip_path_prefix(
    std::string_view path,
    std::string_view prefix
) -> StringId;

[[nodiscard]]
auto resolve_under_root(
    std::string_view path,
    std::string_view source_root,
    std::string_view target_root
) -> std::optional<StringId>;

[[nodiscard]]
auto make_source_relative(
    std::string_view path,
    std::string_view source_to_root,
    std::string_view source_dir
) -> StringId;

} // namespace pup
