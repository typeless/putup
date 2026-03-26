// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/format_to.hpp"
#include "pup/core/string.hpp"

#include <string_view>

namespace pup {

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
