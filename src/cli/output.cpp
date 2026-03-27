// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/output.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <ranges>

namespace pup::cli {

namespace {
constexpr auto ASCII_CONTROL_CHAR_MAX = static_cast<unsigned char>(0x1F);
}

auto remove_empty_directories(
    Vec<StringId> const& output_dir_ids,
    std::string_view build_dir,
    std::string_view source_dir,
    OutputMode mode
) -> std::size_t
{
    auto removed = std::size_t { 0 };

    auto dirs = Vec<String> {};
    dirs.reserve(output_dir_ids.size());
    for (auto id : output_dir_ids) {
        dirs.push_back(String { global_pool().get(id) });
    }
    std::ranges::sort(dirs);
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
    std::ranges::sort(dirs, std::greater {}, [](auto const& p) {
        return p.size();
    });

    for (auto const& dir : dirs) {
        if (dir == source_dir) {
            continue;
        }

        auto rel = pup::global_pool().get(pup::path::relative(dir, build_dir));
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

auto escape_dot_label(std::string_view s) -> StringId
{
    auto buf = Buf {};
    buf.reserve(s.size());
    for (auto c : s) {
        if (c == '"' || c == '\\') {
            buf += '\\';
        }
        buf += c;
    }
    return buf.intern(global_pool());
}

auto escape_json(std::string_view s) -> StringId
{
    auto buf = Buf {};
    buf.reserve(s.size());
    for (auto c : s) {
        switch (c) {
        case '"':
            buf += "\\\"";
            break;
        case '\\':
            buf += "\\\\";
            break;
        case '\b':
            buf += "\\b";
            break;
        case '\f':
            buf += "\\f";
            break;
        case '\n':
            buf += "\\n";
            break;
        case '\r':
            buf += "\\r";
            break;
        case '\t':
            buf += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) <= ASCII_CONTROL_CHAR_MAX) {
                continue;
            }
            buf += c;
        }
    }
    return buf.intern(global_pool());
}

} // namespace pup::cli
