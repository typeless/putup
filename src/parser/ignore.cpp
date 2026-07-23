// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/parser/ignore.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include <cstddef>
#include <optional>
#include <string_view>

namespace pup::parser {

// =============================================================================
// IgnoreList
// =============================================================================

auto IgnoreList::with_defaults() -> IgnoreList
{
    auto list = IgnoreList {};
    list.add(".git/");
    list.add(".pup/");
    list.add("node_modules/");
    return list;
}

auto IgnoreList::add(std::string_view pattern) -> void
{
    if (auto p = parse_pattern(pattern)) {
        patterns_.push_back(*p);
    }
}

auto IgnoreList::parse_pattern(std::string_view line) -> std::optional<StringId>
{
    if (line.empty() || line[0] == '!') {
        return std::nullopt;
    }

    if (line.back() == '/') {
        line = line.substr(0, line.size() - 1);
        if (line.empty()) {
            return std::nullopt;
        }
    }

    return global_pool().intern(line);
}

auto IgnoreList::is_ignored(std::string_view rel_path) const -> bool
{
    for (auto pattern_id : patterns_) {
        if (match_pattern(global_pool().get(pattern_id), rel_path)) {
            return true;
        }
    }
    return false;
}

auto IgnoreList::is_ignored_dir(std::string_view rel_path) const -> bool
{
    if (rel_path == ".") {
        return is_ignored(rel_path);
    }
    for (auto sep = rel_path.find('/'); sep != std::string_view::npos;
         sep = rel_path.find('/', sep + 1)) {
        if (is_ignored(rel_path.substr(0, sep))) {
            return true;
        }
    }
    return is_ignored(rel_path);
}

auto IgnoreList::match_pattern(std::string_view pattern, std::string_view path_str) -> bool
{
    auto const anchored = pattern.find('/') != std::string_view::npos;
    if (anchored) {
        return glob_match(pattern, path_str);
    }

    auto slash_pos = path_str.rfind('/');
    auto basename = (slash_pos == std::string_view::npos) ? path_str : path_str.substr(slash_pos + 1);
    if (glob_match(pattern, basename)) {
        return true;
    }

    if (pattern.starts_with("**")) {
        return glob_match(pattern, path_str);
    }

    return false;
}

auto IgnoreList::glob_match(std::string_view pattern, std::string_view text) -> bool
{
    return glob_match_recursive(pattern, text);
}

auto IgnoreList::glob_match_recursive(std::string_view pattern, std::string_view text) -> bool
{
    auto pi = std::size_t { 0 };
    auto ti = std::size_t { 0 };

    while (pi < pattern.size() && ti < text.size()) {
        auto const pc = pattern[pi];

        // ** matches any path segments (including none)
        if (pc == '*' && pi + 1 < pattern.size() && pattern[pi + 1] == '*') {
            // Skip **
            pi += 2;
            // Skip optional trailing /
            if (pi < pattern.size() && pattern[pi] == '/') {
                ++pi;
            }

            // If ** is at end, match everything
            if (pi >= pattern.size()) {
                return true;
            }

            // Try matching remainder after zero or more whole segments
            for (auto i = ti;;) {
                if (glob_match_recursive(pattern.substr(pi), text.substr(i))) {
                    return true;
                }
                auto slash = text.find('/', i);
                if (slash == std::string_view::npos) {
                    return false;
                }
                i = slash + 1;
            }
        }

        // * matches any characters except /
        if (pc == '*') {
            ++pi;

            // If * is at end of pattern, match if no more / in text
            if (pi >= pattern.size()) {
                return text.find('/', ti) == std::string_view::npos;
            }

            // Try matching at each position until /
            while (ti < text.size() && text[ti] != '/') {
                if (glob_match_recursive(pattern.substr(pi), text.substr(ti))) {
                    return true;
                }
                ++ti;
            }
            // Also try matching at current position (empty * match)
            return glob_match_recursive(pattern.substr(pi), text.substr(ti));
        }

        // ? matches any single character except /
        if (pc == '?') {
            if (text[ti] == '/') {
                return false;
            }
            ++pi;
            ++ti;
            continue;
        }

        // [...] character class
        if (pc == '[') {
            auto close = pattern.find(']', pi + 1);
            if (close == std::string_view::npos) {
                // No closing bracket, treat as literal
                if (text[ti] != pc) {
                    return false;
                }
                ++pi;
                ++ti;
                continue;
            }

            auto negate = false;
            auto class_start = pi + 1;
            if (class_start < pattern.size() && (pattern[class_start] == '!' || pattern[class_start] == '^')) {
                negate = true;
                ++class_start;
            }

            auto matched = false;
            auto c = text[ti];
            for (auto i = class_start; i < close; ++i) {
                // Check for range a-z
                if (i + 2 < close && pattern[i + 1] == '-') {
                    if (c >= pattern[i] && c <= pattern[i + 2]) {
                        matched = true;
                        break;
                    }
                    i += 2;
                } else if (pattern[i] == c) {
                    matched = true;
                    break;
                }
            }

            if (negate) {
                matched = !matched;
            }

            if (!matched) {
                return false;
            }

            pi = close + 1;
            ++ti;
            continue;
        }

        // Literal character
        if (pc != text[ti]) {
            return false;
        }

        ++pi;
        ++ti;
    }

    // Skip trailing * or **
    while (pi < pattern.size()) {
        if (pattern[pi] == '*') {
            ++pi;
        } else {
            break;
        }
    }

    return pi >= pattern.size() && ti >= text.size();
}

} // namespace pup::parser
