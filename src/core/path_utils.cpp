// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/path_utils.hpp"
#include "pup/core/path.hpp"

#include <algorithm>
#include <optional>

namespace pup {

auto is_path_under(
    std::string_view path_str,
    std::string_view root
) -> bool
{
    auto root_sv = root;

    while (!root_sv.empty() && root_sv.back() == '/') {
        root_sv.remove_suffix(1);
    }

    if (path_str == root_sv) {
        return true;
    }

    if (!path_str.starts_with(root_sv)) {
        return false;
    }

    return path_str[root_sv.size()] == '/';
}

auto relative_to_root(
    std::string_view path_str,
    std::string_view root
) -> String
{
    if (!is_path_under(path_str, root)) {
        return {};
    }

    auto root_sv = root;

    while (!root_sv.empty() && root_sv.back() == '/') {
        root_sv.remove_suffix(1);
    }

    if (path_str == root_sv) {
        return {};
    }

    return String { path_str.substr(root_sv.size() + 1) };
}

auto is_path_in_scope(
    std::string_view path,
    std::string_view scope
) -> bool
{
    if (scope.empty()) {
        return true;
    }

    while (!scope.empty() && (scope.back() == '/' || scope.back() == '\\')) {
        scope.remove_suffix(1);
    }

    if (scope.empty()) {
        return true;
    }

    if (!path.starts_with(scope)) {
        return false;
    }

    if (path.size() == scope.size()) {
        return true;
    }

    auto const sep = path[scope.size()];
    return sep == '/' || sep == '\\';
}

auto is_path_in_any_scope(
    std::string_view path,
    Vec<String> const& scopes
) -> bool
{
    if (scopes.empty()) {
        return true;
    }

    return std::ranges::any_of(scopes, [path](auto const& scope) {
        return is_path_in_scope(path, scope);
    });
}

auto compute_source_to_root(std::string_view source_dir) -> String
{
    if (source_dir.empty()) {
        return {};
    }
    auto result = String {};
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
    std::string_view path_sv,
    std::string_view source_to_root,
    std::string_view source_dir
) -> String
{
    if (path_sv.empty() || path_sv[0] == '/') {
        return String { path_sv };
    }
    if (path_sv.size() >= 2 && path_sv[0] == '.' && path_sv[1] == '.') {
        if (!source_to_root.empty() && !source_dir.empty()) {
            auto result = String { source_to_root };
            result += path_sv;
            return result;
        }
        return String { path_sv };
    }
    if (source_to_root.empty()) {
        return String { path_sv };
    }
    auto dir_prefix = String { source_dir };
    dir_prefix += '/';
    if (path_sv.starts_with(std::string_view { dir_prefix })) {
        return String { path_sv.substr(dir_prefix.size()) };
    }
    if (path_sv == source_dir) {
        return String { "." };
    }
    auto result = String { source_to_root };
    result += path_sv;
    return result;
}

auto strip_path_prefix(
    std::string_view path_sv,
    std::string_view prefix
) -> String
{
    if (prefix.empty()) {
        return String { path_sv };
    }
    auto prefix_with_slash = String { prefix };
    prefix_with_slash += '/';
    if (path_sv.starts_with(prefix_with_slash)) {
        return String { path_sv.substr(prefix_with_slash.size()) };
    }
    return String { path_sv };
}

auto resolve_under_root(
    std::string_view path_sv,
    std::string_view source_root,
    std::string_view target_root
) -> std::optional<String>
{
    if (!path_sv.starts_with("..")) {
        return std::nullopt;
    }

    auto abs_path = path::normalize(path::join(source_root, path_sv));
    auto target_prefix = path::normalize(target_root);
    auto rel = path::relative(abs_path, target_prefix);

    if (!rel.empty() && !rel.starts_with("..")) {
        return rel;
    }
    return std::nullopt;
}

} // namespace pup
