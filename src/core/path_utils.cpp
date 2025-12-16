// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/core/path_utils.hpp"

#include <algorithm>

namespace pup {

auto is_path_under(
    std::filesystem::path const& path,
    std::filesystem::path const& root) -> bool
{
    auto path_str = path.string();
    auto root_str = root.string();

    // Handle trailing slash
    while (!root_str.empty() && (root_str.back() == '/' || root_str.back() == '\\')) {
        root_str.pop_back();
    }

    // Exact match
    if (path_str == root_str) {
        return true;
    }

    // Check prefix with directory boundary
    if (!path_str.starts_with(root_str)) {
        return false;
    }

    // Ensure we match at directory boundary
    auto const sep = path_str[root_str.size()];
    return sep == '/' || sep == '\\';
}

auto relative_to_root(
    std::filesystem::path const& path,
    std::filesystem::path const& root) -> std::string
{
    if (!is_path_under(path, root)) {
        return "";
    }

    auto path_str = path.string();
    auto root_str = root.string();

    // Handle trailing slash
    while (!root_str.empty() && (root_str.back() == '/' || root_str.back() == '\\')) {
        root_str.pop_back();
    }

    // Exact match returns empty
    if (path_str == root_str) {
        return "";
    }

    // Skip root prefix and separator
    return path_str.substr(root_str.size() + 1);
}

auto is_path_in_scope(
    std::string_view path,
    std::string_view scope) -> bool
{
    if (scope.empty()) {
        return true;
    }

    if (!path.starts_with(scope)) {
        return false;
    }

    if (path.size() == scope.size()) {
        return true;
    }

    // Ensure directory boundary
    auto const sep = path[scope.size()];
    return sep == '/' || sep == '\\';
}

auto is_path_in_any_scope(
    std::string_view path,
    std::vector<std::string> const& scopes) -> bool
{
    if (scopes.empty()) {
        return true;
    }

    return std::ranges::any_of(scopes, [path](auto const& scope) {
        return is_path_in_scope(path, scope);
    });
}

} // namespace pup
