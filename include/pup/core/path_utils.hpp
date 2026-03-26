// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string.hpp"
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
) -> String;

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
auto compute_source_to_root(std::string_view source_dir) -> String;

[[nodiscard]]
auto strip_path_prefix(
    std::string_view path,
    std::string_view prefix
) -> String;

[[nodiscard]]
auto resolve_under_root(
    std::string_view path,
    std::string_view source_root,
    std::string_view target_root
) -> std::optional<String>;

[[nodiscard]]
auto make_source_relative(
    std::string_view path,
    std::string_view source_to_root,
    std::string_view source_dir
) -> String;

} // namespace pup
