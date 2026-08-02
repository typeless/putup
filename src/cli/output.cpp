// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/output.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/print.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/vec.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string_view>

namespace pup::cli {

namespace {
constexpr auto ASCII_CONTROL_CHAR_MAX = static_cast<unsigned char>(0x1F);
}

namespace {

auto contains_path(Vec<StringId> const& sorted, std::string_view path) -> bool
{
    return std::binary_search(sorted.begin(), sorted.end(), global_pool().intern(path), handle_less);
}

// Empty once everything still in it has itself been taken. Directories are visited
// deepest-first, so a nested one is already in `removed` by the time its parent is asked.
auto would_be_empty(std::string_view dir, Vec<StringId> const& removed) -> bool
{
    auto entries = pup::platform::DirEntries {};
    if (!pup::platform::read_directory(dir, entries)) {
        return false;
    }
    for (auto const& entry : entries.entries) {
        auto child = global_pool().get(pup::path::join(dir, entry.name));
        if (!contains_path(removed, child)) {
            return false;
        }
    }
    return true;
}

} // namespace

auto remove_empty_directories(
    Vec<StringId> const& output_dir_ids,
    std::string_view build_dir,
    std::string_view source_dir,
    OutputMode mode,
    std::string_view variant_name,
    Vec<StringId> const& removed_files
) -> RemoveResult
{
    auto result = RemoveResult {};
    auto removed = removed_files;
    std::ranges::sort(removed, handle_less);

    auto dirs = Vec<StringId> {};
    dirs.reserve(output_dir_ids.size());
    for (auto id : output_dir_ids) {
        dirs.push_back(id);
    }
    std::ranges::sort(dirs, handle_less);
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
    std::ranges::sort(dirs, std::greater {}, [](auto id) {
        return global_pool().get(id).size();
    });

    for (auto dir_id : dirs) {
        auto dir = global_pool().get(dir_id);
        if (dir == source_dir) {
            continue;
        }

        auto rel = pup::global_pool().get(pup::path::relative(dir, build_dir));
        if (rel.starts_with("..")) {
            continue;
        }

        if (!pup::platform::exists(dir)) {
            continue;
        }
        // A dry run removes nothing, so no directory ever becomes empty on disk. Asking
        // whether everything left would itself be removed is the only way to report the
        // directories a real run would take (#258).
        if (!(mode.dry_run ? would_be_empty(dir, removed) : pup::platform::is_empty(dir))) {
            continue;
        }

        if (mode.dry_run) {
            print("[{}] Would remove empty dir: {}\n", variant_name, dir);
            ++result.removed_count;
            auto pos = std::ranges::lower_bound(removed, dir_id, handle_less);
            removed.insert(removed.begin() + (pos - removed.begin()), dir_id);
        } else if (auto r = pup::platform::remove_file(dir); r) {
            ++result.removed_count;
            if (mode.verbose) {
                print("[{}] Removed empty dir: {}\n", variant_name, dir);
            }
        } else {
            eprint("[{}] {}\n", variant_name, r.error().msg());
            ++result.error_count;
        }
    }
    return result;
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
