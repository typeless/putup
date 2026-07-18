// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/parser/ignore.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/file_io.hpp"
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace pup::parser {

// =============================================================================
// IgnoreList
// =============================================================================

auto IgnoreList::load(std::string_view path) -> Result<IgnoreList>
{
    auto content = Buf {};
    if (!pup::platform::read_file(path, content)) {
        auto err = Buf {};
        err.fmt("Failed to open ignore file: {}", path);
        return make_error<IgnoreList>(ErrorCode::IoError, err.view());
    }

    auto list = IgnoreList::with_defaults();
    auto sv = content.view();

    while (!sv.empty()) {
        auto nl = sv.find('\n');
        auto raw = (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
        sv = (nl == std::string_view::npos) ? std::string_view {} : sv.substr(nl + 1);

        // Strip \r
        if (!raw.empty() && raw.back() == '\r') {
            raw.remove_suffix(1);
        }

        if (raw.empty()) {
            continue;
        }

        auto start = raw.find_first_not_of(" \t");
        if (start == std::string_view::npos) {
            continue;
        }
        raw = raw.substr(start);

        if (raw[0] == '#') {
            continue;
        }

        auto line = raw;
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            if (line.size() >= 2 && line[line.size() - 2] == '\\') {
                break;
            }
            line.remove_suffix(1);
        }

        if (line.empty()) {
            continue;
        }

        list.add(line);
    }

    return list;
}

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
        patterns_.push_back(std::move(*p));
    }
}

auto IgnoreList::parse_pattern(std::string_view line) -> std::optional<IgnorePattern>
{
    if (line.empty()) {
        return std::nullopt;
    }

    auto p = IgnorePattern {};

    // Check for negation
    if (line[0] == '!') {
        p.negated = true;
        line = line.substr(1);
        if (line.empty()) {
            return std::nullopt;
        }
    }

    // Check for directory-only (trailing /)
    if (!line.empty() && line.back() == '/') {
        p.dir_only = true;
        line = line.substr(0, line.size() - 1);
        if (line.empty()) {
            return std::nullopt;
        }
    }

    // Check if anchored (contains / not at end)
    // A pattern is anchored if it contains a / in the middle
    auto slash_pos = line.find('/');
    if (slash_pos != std::string_view::npos) {
        p.anchored = true;
    }

    p.pattern = global_pool().intern(line);
    return p;
}

auto IgnoreList::is_ignored(std::string_view rel_path) const -> bool
{
    auto ignored = false;

    // Apply patterns in order, later patterns can override earlier ones
    for (auto const& p : patterns_) {
        if (match_pattern(p, rel_path)) {
            ignored = !p.negated;
        }
    }

    return ignored;
}

auto IgnoreList::match_pattern(IgnorePattern const& p, std::string_view path_str) const -> bool
{
    auto pattern_sv = global_pool().get(p.pattern);
    if (p.anchored) {
        return glob_match(pattern_sv, path_str);
    }

    auto slash_pos = path_str.rfind('/');
    auto basename = (slash_pos == std::string_view::npos) ? path_str : path_str.substr(slash_pos + 1);
    if (glob_match(pattern_sv, basename)) {
        return true;
    }

    if (pattern_sv.starts_with("**")) {
        return glob_match(pattern_sv, path_str);
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

            // Try matching remainder at every position
            for (auto i = ti; i <= text.size(); ++i) {
                if (glob_match_recursive(pattern.substr(pi), text.substr(i))) {
                    return true;
                }
            }
            return false;
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
