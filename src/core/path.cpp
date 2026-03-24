// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/path.hpp"

#include <vector>

namespace pup::path {

auto join(std::string_view a, std::string_view b) -> String
{
    if (b.empty()) {
        return String { a };
    }
    if (a.empty() || is_absolute(b)) {
        return String { b };
    }

    auto result = String { a };
    if (result.back() != '/') {
        result += '/';
    }
    result += b;
    return result;
}

auto parent(std::string_view p) -> std::string_view
{
    if (p.empty()) {
        return {};
    }

    // Trim trailing slash (except root "/")
    auto end = p.size();
    while (end > 1 && p[end - 1] == '/') {
        --end;
    }

    auto pos = p.rfind('/', end - 1);
    if (pos == std::string_view::npos) {
        return {};
    }
    if (pos == 0) {
        return p.substr(0, 1); // "/"
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
    // Drive letter: C:/ or C:\.
    if (p.size() >= 3 && p[1] == ':' && (p[2] == '/' || p[2] == '\\')) {
        auto c = p[0];
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }
#endif
    return false;
}

auto normalize(std::string_view p) -> String
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
            // Cannot go above root — absorb
        } else {
            parts.push_back(part);
        }
        start = end + 1;
    }

    if (parts.empty()) {
        return absolute ? String { "/" } : String { "." };
    }

    auto result = String {};
    if (absolute) {
        result = String { "/" };
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0 || absolute) {
            if (!result.empty() && result.back() != '/') {
                result += '/';
            }
        }
        result += parts[i];
    }
    return result;
}

auto relative(std::string_view target, std::string_view base) -> String
{
    if (target == base) {
        return String { "." };
    }

    // Split both paths into components
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

    // Find common prefix length
    auto common = std::size_t { 0 };
    auto max_common = std::min(target_parts.size(), base_parts.size());
    while (common < max_common && target_parts[common] == base_parts[common]) {
        ++common;
    }

    auto result = String {};
    // Go up from base to common ancestor
    for (auto i = common; i < base_parts.size(); ++i) {
        if (!result.empty()) {
            result += '/';
        }
        result += "..";
    }
    // Append remaining target path
    for (auto i = common; i < target_parts.size(); ++i) {
        if (!result.empty()) {
            result += '/';
        }
        result += target_parts[i];
    }

    return result.empty() ? String { "." } : result;
}

} // namespace pup::path
