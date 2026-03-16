// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/output.hpp"
#include "pup/core/path.hpp"
#include "pup/platform/file_io.hpp"

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
    std::vector<std::string> const& output_dirs,
    std::string const& build_dir,
    std::string const& source_dir,
    OutputMode mode
) -> std::size_t
{
    auto removed = std::size_t { 0 };

    auto dirs = output_dirs;
    std::ranges::sort(dirs);
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
    std::ranges::sort(dirs, std::greater {}, [](auto const& p) {
        return p.size();
    });

    for (auto const& dir : dirs) {
        if (dir == source_dir) {
            continue;
        }

        auto rel = pup::path::relative(dir, build_dir);
        if (rel.starts_with("..")) {
            continue;
        }

        if (!pup::platform::exists(dir) || !pup::platform::is_empty(dir)) {
            continue;
        }

        if (mode.dry_run) {
            printf("Would remove empty dir: %s\n", dir.c_str());
        } else {
            (void)pup::platform::remove_file(dir);
            ++removed;
            if (mode.verbose) {
                printf("Removed empty dir: %s\n", dir.c_str());
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
