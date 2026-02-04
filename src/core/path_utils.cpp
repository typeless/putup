// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/path_utils.hpp"

#include <algorithm>
#include <optional>

namespace pup {

auto is_path_under(
    std::filesystem::path const& path,
    std::filesystem::path const& root
) -> bool
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
    std::filesystem::path const& root
) -> std::string
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
    std::string_view scope
) -> bool
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
    std::vector<std::string> const& scopes
) -> bool
{
    if (scopes.empty()) {
        return true;
    }

    return std::ranges::any_of(scopes, [path](auto const& scope) {
        return is_path_in_scope(path, scope);
    });
}

auto compute_source_to_root(std::string_view source_dir) -> std::string
{
    if (source_dir.empty()) {
        return {};
    }
    auto result = std::string {};
    auto pos = std::size_t { 0 };
    while (pos < source_dir.size()) {
        auto slash = source_dir.find('/', pos);
        auto segment = slash == std::string_view::npos
            ? source_dir.substr(pos)
            : source_dir.substr(pos, slash - pos);
        if (!segment.empty() && segment != ".") {
            result += "../";
        }
        pos = slash == std::string_view::npos ? source_dir.size() : slash + 1;
    }
    return result;
}

auto make_source_relative(
    std::string_view path,
    std::string_view source_to_root,
    std::string_view source_dir
) -> std::string
{
    if (path.empty() || path[0] == '/') {
        return std::string { path };
    }
    if (path.size() >= 2 && path[0] == '.' && path[1] == '.') {
        if (!source_to_root.empty() && !source_dir.empty()) {
            return std::string { source_to_root } + std::string { path };
        }
        return std::string { path };
    }
    if (source_to_root.empty()) {
        return std::string { path };
    }
    auto dir_prefix = std::string { source_dir } + "/";
    if (path.starts_with(dir_prefix)) {
        return std::string { path.substr(dir_prefix.size()) };
    }
    if (path == source_dir) {
        return ".";
    }
    return std::string { source_to_root } + std::string { path };
}

auto strip_path_prefix(
    std::string_view path,
    std::string_view prefix
) -> std::string
{
    if (prefix.empty()) {
        return std::string { path };
    }
    auto prefix_with_slash = std::string { prefix } + "/";
    if (path.starts_with(prefix_with_slash)) {
        return std::string { path.substr(prefix_with_slash.size()) };
    }
    return std::string { path };
}

auto resolve_under_root(
    std::string_view path,
    std::filesystem::path const& source_root,
    std::filesystem::path const& target_root
) -> std::optional<std::string>
{
    if (!path.starts_with("..")) {
        return std::nullopt;
    }

    auto abs_path = (source_root / path).lexically_normal();
    auto target_prefix = target_root.lexically_normal();
    auto rel = abs_path.lexically_relative(target_prefix);

    if (!rel.empty() && !rel.string().starts_with("..")) {
        return rel.string();
    }
    return std::nullopt;
}

} // namespace pup
