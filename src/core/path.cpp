// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/path.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"

#include "pup/core/vec.hpp"
#include <algorithm>
#include <cstddef>
#include <string_view>

namespace pup::path {

namespace {

// The prefix `..` cannot escape and normalization reproduces verbatim: 1 for "/", 3 for "C:/".
auto root_length(std::string_view p) -> std::size_t
{
    if (p.empty()) {
        return 0;
    }
    if (p[0] == '/') {
        return 1;
    }
#ifdef _WIN32
    if (p.size() >= 3 && p[1] == ':' && (p[2] == '/' || p[2] == '\\')) {
        auto c = p[0];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            return 3;
        }
    }
#endif
    return 0;
}

auto append_root(Buf& out, std::string_view p, std::size_t root_len) -> void
{
    out.append(p.substr(0, root_len - 1));
    out += '/';
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
    if (a.back() != '/') {
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
    while (end > root_len && end > 1 && p[end - 1] == '/') {
        --end;
    }
    if (end <= root_len) {
        return p.substr(0, root_len);
    }

    auto pos = p.rfind('/', end - 1);
    if (pos == std::string_view::npos) {
        return {};
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
    auto pos = p.rfind('/');
    if (pos == std::string_view::npos) {
        return p;
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

auto is_absolute(std::string_view p) -> bool
{
    return root_length(p) != 0;
}

auto is_root(std::string_view p) -> bool
{
    auto const len = root_length(p);
    return len != 0 && p.size() == len;
}

auto normalize(std::string_view p) -> StringId
{
    auto parts = Vec<std::string_view> {};
    auto const root_len = root_length(p);
    auto start = root_len;
    auto absolute = root_len != 0;

    while (start < p.size()) {
        auto end = p.find('/', start);
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
    auto& pool = global_pool();

    if (target == base) {
        return pool.intern(".");
    }

    auto split = [](std::string_view p) {
        auto parts = Vec<std::string_view> {};
        auto start = std::size_t { 0 };
        while (start < p.size()) {
            auto end = p.find('/', start);
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
