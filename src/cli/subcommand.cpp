// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/subcommand.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>
#include <string_view>

namespace pup::cli {

namespace {

#ifdef _WIN32
constexpr auto LIST_SEPARATOR = ';';
constexpr auto EXE_SUFFIX = std::string_view { ".exe" };
#else
constexpr auto LIST_SEPARATOR = ':';
constexpr auto EXE_SUFFIX = std::string_view {};
#endif

constexpr auto PREFIX = std::string_view { "putup-" };

auto is_bare_name(std::string_view name) -> bool
{
    auto const is_name_char = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '_' || c == '-' || c == '.';
    };
    return !name.empty() && std::ranges::all_of(name, is_name_char);
}

} // namespace

auto find_subcommand(std::string_view name, std::string_view search_path) -> StringId
{
    if (!is_bare_name(name)) {
        return StringId::Empty;
    }

    auto filename = Buf {};
    filename += PREFIX;
    filename += name;
    filename += EXE_SUFFIX;

    auto& pool = global_pool();
    while (!search_path.empty()) {
        auto end = search_path.find(LIST_SEPARATOR);
        auto entry = search_path.substr(0, end);
        search_path = end == std::string_view::npos ? std::string_view {}
                                                    : search_path.substr(end + 1);
        if (entry.empty()) {
            continue;
        }
        auto candidate = path::join(entry, filename.view());
        if (platform::is_file(pool.get(candidate))) {
            return candidate;
        }
    }

    return StringId::Empty;
}

} // namespace pup::cli
