// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/path.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"

#include <vector>

namespace pup::path {

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

    auto end = p.size();
    while (end > 1 && p[end - 1] == '/') {
        --end;
    }

    auto pos = p.rfind('/', end - 1);
    if (pos == std::string_view::npos) {
        return {};
    }
    if (pos == 0) {
        return p.substr(0, 1);
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
    if (p.empty()) {
        return false;
    }
    if (p[0] == '/') {
        return true;
    }
#ifdef _WIN32
    if (p.size() >= 3 && p[1] == ':' && (p[2] == '/' || p[2] == '\\')) {
        auto c = p[0];
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }
#endif
    return false;
}

auto normalize(std::string_view p) -> StringId
{
    auto parts = std::vector<std::string_view> {};
    auto start = std::size_t { 0 };
    auto absolute = is_absolute(p);

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
        return pool.intern(absolute ? "/" : ".");
    }

    auto buf = Buf {};
    if (absolute) {
        buf += '/';
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
        auto parts = std::vector<std::string_view> {};
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
