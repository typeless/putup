// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pup {

[[nodiscard]]
auto is_path_under(
    std::string const& path,
    std::string const& root
) -> bool;

[[nodiscard]]
auto relative_to_root(
    std::string const& path,
    std::string const& root
) -> std::string;

[[nodiscard]]
auto is_path_in_scope(
    std::string_view path,
    std::string_view scope
) -> bool;

[[nodiscard]]
auto is_path_in_any_scope(
    std::string_view path,
    std::vector<std::string> const& scopes
) -> bool;

[[nodiscard]]
auto compute_source_to_root(std::string_view source_dir) -> std::string;

[[nodiscard]]
auto strip_path_prefix(
    std::string_view path,
    std::string_view prefix
) -> std::string;

[[nodiscard]]
auto resolve_under_root(
    std::string_view path,
    std::string const& source_root,
    std::string const& target_root
) -> std::optional<std::string>;

[[nodiscard]]
auto make_source_relative(
    std::string_view path,
    std::string_view source_to_root,
    std::string_view source_dir
) -> std::string;

} // namespace pup
