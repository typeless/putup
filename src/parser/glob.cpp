// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/parser/glob.hpp"

#include <algorithm>

namespace pup::parser {

// =============================================================================
// Glob
// =============================================================================

Glob::Glob(std::string_view pattern)
    : pattern_(pattern)
{
    for (auto c : pattern) {
        if (c == '*' || c == '?' || c == '[') {
            has_wildcards_ = true;
            break;
        }
    }

    // Check for **
    auto pos = std::size_t { pattern.find("**") };
    has_double_star_ = (pos != std::string_view::npos);
}

auto Glob::matches(std::string_view filename) const -> bool
{
    if (!has_wildcards_)
        return pattern_ == filename;
    return match_impl(pattern_, filename);
}

auto Glob::match_impl(std::string_view pattern, std::string_view text) const -> bool
{
    // Handle recursive ** patterns with a more straightforward approach
    // by trying to match at each position

    return match_recursive(pattern, text);
}

auto Glob::match_recursive(std::string_view pattern, std::string_view text) const -> bool
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
            if (pi < pattern.size() && pattern[pi] == '/')
                ++pi;

            // If ** is at end, match everything
            if (pi >= pattern.size())
                return true;

            // Try matching remainder at every position
            for (auto i = ti; i <= text.size(); ++i) {
                if (match_recursive(pattern.substr(pi), text.substr(i)))
                    return true;
            }
            return false;
        }

        // * matches any characters except /
        if (pc == '*') {
            ++pi;

            // If * is at end of pattern, match if no more / in text
            if (pi >= pattern.size())
                return text.find('/', ti) == std::string_view::npos;

            // Try matching at each position until /
            while (ti < text.size() && text[ti] != '/') {
                if (match_recursive(pattern.substr(pi), text.substr(ti)))
                    return true;
                ++ti;
            }
            // Also try matching at current position (empty * match)
            return match_recursive(pattern.substr(pi), text.substr(ti));
        }

        // ? matches any single character except /
        if (pc == '?') {
            if (text[ti] == '/')
                return false;
            ++pi;
            ++ti;
            continue;
        }

        // [...] character class
        if (pc == '[') {
            auto bracket_pattern = pattern.substr(pi);
            if (!match_bracket(bracket_pattern, text[ti]))
                return false;
            pi = pattern.size() - bracket_pattern.size();
            ++ti;
            continue;
        }

        // Literal match
        if (pc != text[ti])
            return false;

        ++pi;
        ++ti;
    }

    // Handle trailing pattern
    while (pi < pattern.size()) {
        if (pattern[pi] == '*') {
            ++pi;
            // Skip **
            if (pi < pattern.size() && pattern[pi] == '*')
                ++pi;
            // Skip trailing /
            if (pi < pattern.size() && pattern[pi] == '/')
                ++pi;
        } else {
            break;
        }
    }

    return pi >= pattern.size() && ti >= text.size();
}

auto Glob::match_bracket(std::string_view& pattern, char c) const -> bool
{
    if (pattern.empty() || pattern[0] != '[')
        return false;

    auto negate = false;
    auto pos = std::size_t { 1 };

    if (pos < pattern.size() && (pattern[pos] == '!' || pattern[pos] == '^')) {
        negate = true;
        ++pos;
    }

    auto matched = false;
    auto prev = char { 0 };

    while (pos < pattern.size() && pattern[pos] != ']') {
        auto const pc = pattern[pos];

        // Range: a-z
        if (pc == '-' && prev != 0 && pos + 1 < pattern.size() && pattern[pos + 1] != ']') {
            auto const end = pattern[pos + 1];
            if (c >= prev && c <= end)
                matched = true;
            pos += 2;
            prev = 0;
            continue;
        }

        if (pc == c)
            matched = true;
        prev = pc;
        ++pos;
    }

    // Skip closing bracket
    if (pos < pattern.size() && pattern[pos] == ']')
        ++pos;

    pattern = pattern.substr(pos);
    return negate ? !matched : matched;
}

// =============================================================================
// Glob expansion functions
// =============================================================================

auto glob_expand(
    std::string_view pattern,
    std::filesystem::path const& base_dir,
    GlobOptions const& options) -> Result<std::vector<std::string>>
{
    namespace fs = std::filesystem;

    auto results = std::vector<std::string> {};

    // Check if pattern has wildcards
    if (!has_glob_chars(pattern)) {
        // Literal path - just check if it exists
        auto path = fs::path { base_dir / pattern };
        if (fs::exists(path))
            results.emplace_back(pattern);
        return results;
    }

    // Split into directory and pattern parts
    auto [dir_part, file_pattern] = glob_split_path(pattern);
    auto search_dir = fs::path { dir_part.empty() ? base_dir : base_dir / dir_part };

    if (!fs::exists(search_dir) || !fs::is_directory(search_dir))
        return results;

    auto glob = Glob { file_pattern };

    // Check if we need recursive search
    auto const is_recursive = glob.is_recursive() && options.recursive;

    auto iterate = [&](auto const& entry) {
        auto const& path = entry.path();
        auto filename = path.filename().string();

        // Skip hidden files unless requested
        if (!options.include_hidden && !filename.empty() && filename[0] == '.')
            return;

        // For recursive, match against relative path
        if (is_recursive) {
            auto rel = fs::relative(path, search_dir);
            if (glob.matches(rel.string())) {
                auto result_path = dir_part.empty() ? rel.string() : std::string { dir_part } + "/" + rel.string();
                results.push_back(result_path);
            }
        } else {
            // Match just the filename
            if (glob.matches(filename)) {
                auto result_path = dir_part.empty() ? filename : std::string { dir_part } + "/" + filename;
                results.push_back(result_path);
            }
        }
    };

    std::error_code ec;
    if (is_recursive) {
        for (auto const& entry : fs::recursive_directory_iterator(search_dir, ec)) {
            if (ec)
                break;
            iterate(entry);
        }
    } else {
        for (auto const& entry : fs::directory_iterator(search_dir, ec)) {
            if (ec)
                break;
            iterate(entry);
        }
    }

    // Sort for consistent output
    std::sort(results.begin(), results.end());
    return results;
}

auto glob_expand_all(
    std::vector<std::string> const& patterns,
    std::filesystem::path const& base_dir,
    GlobOptions const& options) -> Result<GlobResult>
{
    auto result = GlobResult {};

    for (auto const& pattern : patterns) {
        if (pattern.empty())
            continue;

        // Check for exclusion pattern
        if (pattern[0] == '!') {
            auto exclude_pattern = pattern.substr(1);
            auto excluded = glob_expand(exclude_pattern, base_dir, options);
            if (!excluded)
                return pup::unexpected<Error>(excluded.error());
            for (auto& path : *excluded)
                result.exclusions.push_back(std::move(path));
        } else {
            auto matches = glob_expand(pattern, base_dir, options);
            if (!matches)
                return pup::unexpected<Error>(matches.error());
            for (auto& path : *matches)
                result.matches.push_back(std::move(path));
        }
    }

    // Remove excluded files from matches
    for (auto const& excl : result.exclusions) {
        result.matches.erase(
            std::remove(result.matches.begin(), result.matches.end(), excl),
            result.matches.end());
    }

    return result;
}

auto glob_split_path(std::string_view pattern)
    -> std::pair<std::string_view, std::string_view>
{
    // Find the last / before any glob characters
    auto glob_pos = std::string_view::npos;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        auto c = pattern[i];
        if (c == '*' || c == '?' || c == '[') {
            glob_pos = i;
            break;
        }
    }

    if (glob_pos == std::string_view::npos) {
        // No globs - find last /
        auto last_slash = pattern.rfind('/');
        if (last_slash == std::string_view::npos)
            return { {}, pattern };
        return { pattern.substr(0, last_slash), pattern.substr(last_slash + 1) };
    }

    // Find / before the first glob
    auto slash_before_glob = pattern.substr(0, glob_pos).rfind('/');
    if (slash_before_glob == std::string_view::npos)
        return { {}, pattern };

    return { pattern.substr(0, slash_before_glob), pattern.substr(slash_before_glob + 1) };
}

auto has_glob_chars(std::string_view pattern) -> bool
{
    return std::ranges::any_of(pattern, [](char c) {
        return c == '*' || c == '?' || c == '[';
    });
}

auto path_basename(std::string_view path) -> std::string_view
{
    auto last_slash = path.rfind('/');
    if (last_slash == std::string_view::npos)
        return path;
    return path.substr(last_slash + 1);
}

auto path_stem(std::string_view path) -> std::string_view
{
    auto base = path_basename(path);
    auto dot = base.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return base;
    return base.substr(0, dot);
}

auto path_extension(std::string_view path) -> std::string_view
{
    auto base = path_basename(path);
    auto dot = base.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return {};
    return base.substr(dot + 1);
}

auto path_directory(std::string_view path) -> std::string_view
{
    auto last_slash = path.rfind('/');
    if (last_slash == std::string_view::npos)
        return {};
    return path.substr(0, last_slash);
}

} // namespace pup::parser
