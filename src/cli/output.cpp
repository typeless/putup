// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/output.hpp"

#include <algorithm>
#include <functional>
#include <ranges>
#include <vector>

#include <fmt/core.h>

namespace pup::cli {

auto remove_file(std::filesystem::path const& path, OutputMode mode) -> bool
{
    if (!std::filesystem::exists(path)) {
        return false;
    }

    if (mode.dry_run) {
        fmt::print("Would remove: {}\n", path.string());
        return true;
    }

    if (mode.verbose) {
        fmt::print("Removing: {}\n", path.string());
    }

    auto ec = std::error_code {};
    return std::filesystem::remove(path, ec);
}

auto remove_empty_dir(
    std::filesystem::path const& dir,
    std::filesystem::path const& root_guard,
    OutputMode mode) -> bool
{
    if (dir == root_guard) {
        return false;
    }

    if (!std::filesystem::exists(dir) || !std::filesystem::is_empty(dir)) {
        return false;
    }

    if (mode.dry_run) {
        fmt::print("Would remove empty dir: {}\n", dir.string());
        return true;
    }

    if (mode.verbose) {
        fmt::print("Removing empty dir: {}\n", dir.string());
    }

    auto ec = std::error_code {};
    return std::filesystem::remove(dir, ec);
}

auto remove_empty_directories(
    std::set<std::filesystem::path> const& output_dirs,
    std::filesystem::path const& build_dir,
    std::filesystem::path const& source_dir,
    OutputMode mode) -> std::size_t
{
    auto removed = std::size_t { 0 };

    auto dirs = std::vector<std::filesystem::path>(output_dirs.begin(), output_dirs.end());
    std::ranges::sort(dirs, std::greater{}, [](auto const& p) {
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
            fmt::print("Would remove empty dir: {}\n", dir.string());
        } else {
            std::filesystem::remove(dir);
            ++removed;
            if (mode.verbose) {
                fmt::print("Removed empty dir: {}\n", dir.string());
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
            if (static_cast<unsigned char>(c) < 0x20) {
                continue;
            }
            result += c;
        }
    }
    return result;
}

} // namespace pup::cli
