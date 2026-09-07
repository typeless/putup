// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/parser/glob.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/expected.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/vec.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

namespace pup::parser {

// =============================================================================
// Glob
// =============================================================================

Glob::Glob(std::string_view pattern)
    : pattern_id_(global_pool().intern(pattern))
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
    if (!has_wildcards_) {
        return global_pool().get(pattern_id_) == filename;
    }
    return match_recursive(global_pool().get(pattern_id_), filename);
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
            if (pi < pattern.size() && pattern[pi] == '/') {
                ++pi;
            }

            // If ** is at end, match everything
            if (pi >= pattern.size()) {
                return true;
            }

            // Try matching remainder at every position
            for (auto i = ti; i <= text.size(); ++i) {
                if (match_recursive(pattern.substr(pi), text.substr(i))) {
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
                if (match_recursive(pattern.substr(pi), text.substr(ti))) {
                    return true;
                }
                ++ti;
            }
            // Also try matching at current position (empty * match)
            return match_recursive(pattern.substr(pi), text.substr(ti));
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
            auto bracket_pattern = pattern.substr(pi);
            if (!match_bracket(bracket_pattern, text[ti])) {
                return false;
            }
            pi = pattern.size() - bracket_pattern.size();
            ++ti;
            continue;
        }

        // Literal match
        if (pc != text[ti]) {
            return false;
        }

        ++pi;
        ++ti;
    }

    // Handle trailing pattern
    while (pi < pattern.size()) {
        if (pattern[pi] == '*') {
            ++pi;
            // Skip **
            if (pi < pattern.size() && pattern[pi] == '*') {
                ++pi;
            }
            // Skip trailing /
            if (pi < pattern.size() && pattern[pi] == '/') {
                ++pi;
            }
        } else {
            break;
        }
    }

    return pi >= pattern.size() && ti >= text.size();
}

auto Glob::match_bracket(std::string_view& pattern, char c) const -> bool
{
    if (pattern.empty() || pattern[0] != '[') {
        return false;
    }

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
            if (c >= prev && c <= end) {
                matched = true;
            }
            pos += 2;
            prev = 0;
            continue;
        }

        if (pc == c) {
            matched = true;
        }
        prev = pc;
        ++pos;
    }

    // Skip closing bracket
    if (pos < pattern.size() && pattern[pos] == ']') {
        ++pos;
    }

    pattern = pattern.substr(pos);
    return negate ? !matched : matched;
}

// =============================================================================
// Glob expansion functions
// =============================================================================

auto glob_expand(
    std::string_view pattern,
    std::string_view base_dir,
    GlobOptions const& options
) -> Result<Vec<StringId>>
{
    auto& pool = global_pool();
    auto results = Vec<StringId> {};

    if (!has_glob_chars(pattern)) {
        auto path_sv = pool.get(pup::path::join(base_dir, pattern));
        if (pup::platform::exists(path_sv)) {
            results.push_back(pool.intern(pattern));
        }
        return results;
    }

    auto [dir_part, file_pattern] = glob_split_path(pattern);
    auto search_dir_sv = dir_part.empty() ? base_dir : pool.get(pup::path::join(base_dir, dir_part));

    if (!pup::platform::exists(search_dir_sv) || !pup::platform::is_directory(search_dir_sv)) {
        return results;
    }

    auto glob = Glob { file_pattern };

    auto const is_recursive = glob.is_recursive() && options.recursive;

    if (is_recursive) {
        (void)pup::platform::walk_directory(search_dir_sv, [&](pup::platform::DirEntry const& entry, std::string_view rel_path) -> bool {
            auto name_sv = entry.name;
            if (!options.include_hidden && !name_sv.empty() && name_sv[0] == '.') {
                return false;
            }
            if (glob.matches(rel_path)) {
                auto result_id = dir_part.empty() ? pool.intern(rel_path) : pup::path::join(dir_part, rel_path);
                results.push_back(result_id);
            }
            return true;
        });
    } else {
        auto listing = pup::platform::DirEntries {};
        if (pup::platform::read_directory(search_dir_sv, listing)) {
            for (auto const& entry : listing.entries) {
                auto name_sv = entry.name;
                if (!options.include_hidden && !name_sv.empty() && name_sv[0] == '.') {
                    continue;
                }
                if (glob.matches(name_sv)) {
                    auto result_id = dir_part.empty() ? pool.intern(name_sv) : pup::path::join(dir_part, name_sv);
                    results.push_back(result_id);
                }
            }
        }
    }

    // StringId order is interning order, i.e. readdir order, not path order.
    std::ranges::sort(results, {}, [&pool](StringId id) { return pool.get(id); });
    return results;
}

auto glob_expand_all(
    Vec<StringId> const& patterns,
    std::string_view base_dir,
    GlobOptions const& options
) -> Result<GlobResult>
{
    auto& pool = global_pool();
    auto result = GlobResult {};

    for (auto pattern_id : patterns) {
        auto pattern_sv = pool.get(pattern_id);
        if (pattern_sv.empty()) {
            continue;
        }

        if (pattern_sv[0] == '!') {
            auto exclude_pattern = pattern_sv.substr(1);
            auto excluded = glob_expand(exclude_pattern, base_dir, options);
            if (!excluded) {
                return pup::unexpected<Error>(excluded.error());
            }
            for (auto path_id : *excluded) {
                result.exclusions.push_back(path_id);
            }
        } else {
            auto matches = glob_expand(pattern_sv, base_dir, options);
            if (!matches) {
                return pup::unexpected<Error>(matches.error());
            }
            for (auto path_id : *matches) {
                result.matches.push_back(path_id);
            }
        }
    }

    if (!result.exclusions.empty()) {
        auto excl_sorted = Vec<StringId> { result.exclusions };
        std::sort(excl_sorted.begin(), excl_sorted.end(), pup::handle_less);
        auto& m = result.matches;
        for (auto it = m.begin(); it != m.end();) {
            if (std::binary_search(excl_sorted.begin(), excl_sorted.end(), *it, pup::handle_less)) {
                it = m.erase(it);
            } else {
                ++it;
            }
        }
    }

    return result;
}

auto glob_split_path(
    std::string_view pattern
) -> std::pair<std::string_view, std::string_view>
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
        if (last_slash == std::string_view::npos) {
            return { {}, pattern };
        }
        return { pattern.substr(0, last_slash), pattern.substr(last_slash + 1) };
    }

    // Find / before the first glob
    auto slash_before_glob = pattern.substr(0, glob_pos).rfind('/');
    if (slash_before_glob == std::string_view::npos) {
        return { {}, pattern };
    }

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
    return pup::path::filename(path);
}

auto path_stem(std::string_view path) -> std::string_view
{
    return pup::path::stem(path);
}

auto path_extension(std::string_view path) -> std::string_view
{
    return pup::path::bare_extension(path);
}

auto path_directory(std::string_view path) -> std::string_view
{
    return pup::path::parent(path);
}

auto glob_match_extract(std::string_view pattern, std::string_view filename) -> StringId
{
    // For path patterns (containing /), extract basename of both
    auto pattern_base = path_basename(pattern);
    if (pattern_base.empty()) {
        pattern_base = pattern;
    }

    auto filename_base = path_basename(filename);
    if (filename_base.empty()) {
        filename_base = filename;
    }

    // Find the * in the pattern
    auto star_pos = pattern_base.find('*');
    if (star_pos == std::string_view::npos) {
        return {};
    }

    // Get prefix (before *) and suffix (after *)
    auto prefix = pattern_base.substr(0, star_pos);
    auto suffix = pattern_base.substr(star_pos + 1);

    // Handle ** by treating as single *
    if (!suffix.empty() && suffix[0] == '*') {
        suffix = suffix.substr(1);
    }

    // Verify filename starts with prefix and ends with suffix
    if (!filename_base.starts_with(prefix)) {
        return {};
    }
    if (!filename_base.ends_with(suffix)) {
        return {};
    }

    // Extract the middle portion
    auto match_start = prefix.size();
    auto match_end = filename_base.size() - suffix.size();

    if (match_start > match_end) {
        return {};
    }

    return global_pool().intern(filename_base.substr(match_start, match_end - match_start));
}

} // namespace pup::parser
