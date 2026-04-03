// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/path_utils.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/vec.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>

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
) -> StringId
{
    if (!is_path_under(path_str, root)) {
        return StringId::Empty;
    }

    auto root_sv = root;

    while (!root_sv.empty() && root_sv.back() == '/') {
        root_sv.remove_suffix(1);
    }

    if (path_str == root_sv) {
        return StringId::Empty;
    }

    return global_pool().intern(path_str.substr(root_sv.size() + 1));
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
    Vec<StringId> const& scopes
) -> bool
{
    if (scopes.empty()) {
        return true;
    }

    return std::ranges::any_of(scopes, [path](auto const& scope) {
        return is_path_in_scope(path, global_pool().get(scope));
    });
}

auto compute_source_to_root(std::string_view source_dir) -> StringId
{
    if (source_dir.empty()) {
        return StringId::Empty;
    }
    auto buf = Buf {};
    auto pos = std::size_t { 0 };
    while (pos < source_dir.size()) {
        auto slash = source_dir.find('/', pos);
        auto segment = slash == std::string_view::npos
            ? source_dir.substr(pos)
            : source_dir.substr(pos, slash - pos);
        if (!segment.empty() && segment != ".") {
            buf.append("../");
        }
        pos = slash == std::string_view::npos ? source_dir.size() : slash + 1;
    }
    return buf.intern(global_pool());
}

auto make_source_relative(
    std::string_view path_sv,
    std::string_view source_to_root,
    std::string_view source_dir
) -> StringId
{
    auto& pool = global_pool();

    if (path_sv.empty() || path_sv[0] == '/') {
        return pool.intern(path_sv);
    }
    if (path_sv.size() >= 2 && path_sv[0] == '.' && path_sv[1] == '.') {
        if (!source_to_root.empty() && !source_dir.empty()) {
            auto buf = Buf {};
            buf.append(source_to_root);
            buf.append(path_sv);
            return buf.intern(pool);
        }
        return pool.intern(path_sv);
    }
    if (source_to_root.empty()) {
        return pool.intern(path_sv);
    }

    auto buf = Buf {};
    buf.append(source_dir);
    buf += '/';
    auto dir_prefix = buf.view();
    if (path_sv.starts_with(dir_prefix)) {
        return pool.intern(path_sv.substr(dir_prefix.size()));
    }
    if (path_sv == source_dir) {
        return pool.intern(".");
    }

    auto result_buf = Buf {};
    result_buf.append(source_to_root);
    result_buf.append(path_sv);
    return result_buf.intern(pool);
}

auto strip_path_prefix(
    std::string_view path_sv,
    std::string_view prefix
) -> StringId
{
    auto& pool = global_pool();

    if (prefix.empty()) {
        return pool.intern(path_sv);
    }
    auto buf = Buf {};
    buf.append(prefix);
    buf += '/';
    auto prefix_with_slash = buf.view();
    if (path_sv.starts_with(prefix_with_slash)) {
        return pool.intern(path_sv.substr(prefix_with_slash.size()));
    }
    return pool.intern(path_sv);
}

auto resolve_under_root(
    std::string_view path_sv,
    std::string_view source_root,
    std::string_view target_root
) -> std::optional<StringId>
{
    if (!path_sv.starts_with("..")) {
        return std::nullopt;
    }

    auto& pool = global_pool();
    auto abs_path = path::normalize(pool.get(path::join(source_root, path_sv)));
    auto target_prefix = path::normalize(target_root);
    auto rel = path::relative(pool.get(abs_path), pool.get(target_prefix));

    auto rel_sv = pool.get(rel);
    if (!rel_sv.empty() && !rel_sv.starts_with("..")) {
        return rel;
    }
    return std::nullopt;
}

} // namespace pup
