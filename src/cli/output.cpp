// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/output.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <ranges>
#include <vector>

namespace pup::cli {

namespace {
constexpr auto ASCII_CONTROL_CHAR_MAX = static_cast<unsigned char>(0x1F);
}

auto remove_empty_directories(
    std::set<std::filesystem::path> const& output_dirs,
    std::filesystem::path const& build_dir,
    std::filesystem::path const& source_dir,
    OutputMode mode
) -> std::size_t
{
    auto removed = std::size_t { 0 };

    auto dirs = std::vector<std::filesystem::path>(output_dirs.begin(), output_dirs.end());
    std::ranges::sort(dirs, std::greater {}, [](auto const& p) {
        return p.string().size();
    });

    for (auto const& dir : dirs) {
        if (dir == source_dir) {
            continue;
        }

        auto rel = std::filesystem::relative(dir, build_dir);
        if (rel.string().starts_with("..")) {
            continue;
        }

        if (!std::filesystem::exists(dir) || !std::filesystem::is_empty(dir)) {
            continue;
        }

        if (mode.dry_run) {
            printf("Would remove empty dir: %s\n", dir.string().c_str());
        } else {
            std::filesystem::remove(dir);
            ++removed;
            if (mode.verbose) {
                printf("Removed empty dir: %s\n", dir.string().c_str());
            }
        }
    }
    return removed;
}

auto escape_dot_label(std::string_view s) -> std::string
{
    auto result = std::string {};
    result.reserve(s.size());
    for (auto c : s) {
        if (c == '"' || c == '\\') {
            result += '\\';
        }
        result += c;
    }
    return result;
}

auto escape_json(std::string_view s) -> std::string
{
    auto result = std::string {};
    result.reserve(s.size());
    for (auto c : s) {
        switch (c) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) <= ASCII_CONTROL_CHAR_MAX) {
                continue;
            }
            result += c;
        }
    }
    return result;
}

} // namespace pup::cli
