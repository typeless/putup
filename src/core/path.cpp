// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/path.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"

#include "pup/core/vec.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <string_view>

namespace pup::path {

namespace {

// Windows spells a separator both ways; POSIX spells names with either byte, so `\` is a name byte.
#ifdef _WIN32
constexpr auto separators = std::string_view { "/\\" };
#else
constexpr auto separators = std::string_view { "/" };
#endif

auto is_separator(char c) -> bool
{
    return separators.find(c) != std::string_view::npos;
}

// The prefix `..` cannot escape and normalization reproduces verbatim: 1 "/", 3 "C:/", 2 "//" or "C:".
auto root_length(std::string_view p) -> std::size_t
{
    if (p.empty()) {
        return 0;
    }
#ifdef _WIN32
    if (p.size() >= 2 && is_separator(p[0]) && is_separator(p[1])) {
        return 2;
    }
    if (p.size() >= 2 && p[1] == ':') {
        auto c = p[0];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            return (p.size() >= 3 && is_separator(p[2])) ? 3 : 2;
        }
    }
#endif
    if (is_separator(p[0])) {
        return 1;
    }
    return 0;
}

// A bare drive prefix names no directory to separate from: "C:x" is not the file "C:/x".
auto separates_from_child(std::string_view p) -> bool
{
    return !p.empty() && (is_separator(p.back()) || (p.back() == ':' && is_root(p)));
}

auto append_root(Buf& out, std::string_view p, std::size_t root_len) -> void
{
    auto const root = p.substr(0, root_len);
    for (auto c : root) {
        out += is_separator(c) ? '/' : c;
    }
    if (!separates_from_child(root)) {
        out += '/';
    }
}

// Used only by relative()'s precondition assertion, which a release build compiles out.
[[maybe_unused]]
auto same_root(std::string_view a, std::string_view b) -> bool
{
    auto const len = root_length(a);
    return len == root_length(b) && a.substr(0, len) == b.substr(0, len);
}

} // namespace

auto join(std::string_view a, std::string_view b) -> StringId
{
    if (b.empty()) {
        return global_pool().intern(a);
    }
    if (a.empty() || is_absolute(b)) {
        return global_pool().intern(b);
    }

    auto buf = Buf {};
    buf.append(a);
    if (!separates_from_child(a)) {
        buf += '/';
    }
    buf.append(b);
    return buf.intern(global_pool());
}

auto parent(std::string_view p) -> std::string_view
{
    if (p.empty()) {
        return {};
    }

    auto const root_len = root_length(p);
    if (is_root(p)) {
        return p;
    }

    auto end = p.size();
    while (end > root_len && end > 1 && is_separator(p[end - 1])) {
        --end;
    }
    if (end <= root_len) {
        return p.substr(0, root_len);
    }

    auto pos = p.find_last_of(separators, end - 1);
    if (pos == std::string_view::npos) {
        return p.substr(0, root_len);
    }
    if (pos < root_len) {
        return p.substr(0, root_len);
    }
    return p.substr(0, pos);
}

auto filename(std::string_view p) -> std::string_view
{
    if (p.empty()) {
        return {};
    }
    auto pos = p.find_last_of(separators);
    if (pos == std::string_view::npos) {
        return p.substr(root_length(p));
    }
    return p.substr(pos + 1);
}

auto stem(std::string_view p) -> std::string_view
{
    auto name = filename(p);
    if (name.empty() || name == "." || name == "..") {
        return name;
    }
    auto dot = name.rfind('.');
    if (dot == 0 || dot == std::string_view::npos) {
        return name;
    }
    return name.substr(0, dot);
}

auto extension(std::string_view p) -> std::string_view
{
    auto name = filename(p);
    if (name.empty() || name == "." || name == "..") {
        return {};
    }
    auto dot = name.rfind('.');
    if (dot == 0 || dot == std::string_view::npos) {
        return {};
    }
    return name.substr(dot);
}

auto bare_extension(std::string_view p) -> std::string_view
{
    auto ext = extension(p);
    if (!ext.empty() && ext[0] == '.') {
        ext.remove_prefix(1);
    }
    return ext;
}

auto is_absolute(std::string_view p) -> bool
{
    return root_length(p) != 0;
}

auto is_root(std::string_view p) -> bool
{
    auto const len = root_length(p);
    return len != 0 && p.size() == len;
}

auto is_normal(std::string_view p) -> bool
{
    if (p.empty() || p == ".") {
        return true;
    }

#ifdef _WIN32
    // normalize emits only '/', so the other separator spelling is by definition not normal.
    if (p.find('\\') != std::string_view::npos) {
        return false;
    }
#endif

    auto const root_len = root_length(p);
    auto const body = p.substr(root_len);
    if (!body.empty() && body.back() == '/') {
        return false;
    }

    auto seen_name = false;
    auto start = std::size_t { 0 };
    while (start < body.size()) {
        auto end = body.find('/', start);
        if (end == std::string_view::npos) {
            end = body.size();
        }
        auto part = body.substr(start, end - start);
        if (part.empty() || part == ".") {
            return false;
        }
        if (part == "..") {
            if (root_len != 0 || seen_name) {
                return false;
            }
        } else {
            seen_name = true;
        }
        start = end + 1;
    }
    return true;
}

auto normalize(std::string_view p) -> StringId
{
    auto parts = Vec<std::string_view> {};
    auto const root_len = root_length(p);
    auto start = root_len;
    auto absolute = root_len != 0;

    while (start < p.size()) {
        auto end = p.find_first_of(separators, start);
        if (end == std::string_view::npos) {
            end = p.size();
        }
        auto part = p.substr(start, end - start);
        if (part.empty() || part == ".") {
            // skip
        } else if (part == ".." && !parts.empty() && parts.back() != "..") {
            parts.pop_back();
        } else if (part == ".." && absolute) {
            // Cannot go above root
        } else {
            parts.push_back(part);
        }
        start = end + 1;
    }

    auto& pool = global_pool();

    if (parts.empty()) {
        if (!absolute) {
            return pool.intern(".");
        }
        auto root = Buf {};
        append_root(root, p, root_len);
        return root.intern(pool);
    }

    auto buf = Buf {};
    if (absolute) {
        append_root(buf, p, root_len);
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            buf += '/';
        }
        buf.append(parts[i]);
    }
    return buf.intern(pool);
}

auto relative(std::string_view target, std::string_view base) -> StringId
{
    assert(is_normal(target) && is_normal(base) && "path::relative needs normalized operands");
    assert(same_root(target, base) && "path::relative needs operands rooted the same way");

    auto& pool = global_pool();

    if (target == base) {
        return pool.intern(".");
    }

    auto split = [](std::string_view p) {
        auto parts = Vec<std::string_view> {};
        auto start = std::size_t { 0 };
        while (start < p.size()) {
            auto end = p.find_first_of(separators, start);
            if (end == std::string_view::npos) {
                end = p.size();
            }
            auto part = p.substr(start, end - start);
            if (!part.empty() && part != ".") {
                parts.push_back(part);
            }
            start = end + 1;
        }
        return parts;
    };

    auto target_parts = split(target);
    auto base_parts = split(base);

    auto common = std::size_t { 0 };
    auto max_common = std::min(target_parts.size(), base_parts.size());
    while (common < max_common && target_parts[common] == base_parts[common]) {
        ++common;
    }

    auto buf = Buf {};
    for (auto i = common; i < base_parts.size(); ++i) {
        if (!buf.empty()) {
            buf += '/';
        }
        buf.append("..");
    }
    for (auto i = common; i < target_parts.size(); ++i) {
        if (!buf.empty()) {
            buf += '/';
        }
        buf.append(target_parts[i]);
    }

    if (buf.empty()) {
        return pool.intern(".");
    }
    return buf.intern(pool);
}

} // namespace pup::path
