// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string.hpp"

#include <cstddef>
#include <string_view>

namespace pup {

struct FormatArg {
    enum class Tag : std::uint8_t { StringView, Int, Char };

    Tag tag;
    union {
        std::string_view sv;
        int i;
        char c;
    };

    FormatArg(std::string_view s) : tag(Tag::StringView), sv(s) {} // NOLINT
    FormatArg(String const& s) : tag(Tag::StringView), sv(s) {}    // NOLINT
    FormatArg(char const* s) : tag(Tag::StringView), sv(s) {}      // NOLINT
    FormatArg(int v) : tag(Tag::Int), i(v) {}                      // NOLINT
    FormatArg(char v) : tag(Tag::Char), c(v) {}                    // NOLINT
    FormatArg(std::size_t v) : tag(Tag::Int), i(static_cast<int>(v)) {} // NOLINT
};

auto format_impl(std::string_view pattern, FormatArg const* args, std::size_t count) -> String;

inline auto fmt(std::string_view pattern) -> String
{
    return format_impl(pattern, nullptr, 0);
}

template<typename... Args>
auto fmt(std::string_view pattern, Args const&... args) -> String
{
    FormatArg arg_array[] = { FormatArg(args)... };
    return format_impl(pattern, arg_array, sizeof...(Args));
}

} // namespace pup
