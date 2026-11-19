// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/format_to.hpp"
#include "pup/platform/stream.hpp"

#include <cstddef>
#include <string_view>

namespace pup {

auto print_args(platform::Stream stream, std::string_view pattern, FormatArg const* args, std::size_t count) -> void;

inline auto print(std::string_view text) -> void
{
    platform::write_to(platform::Stream::Out, text);
}

inline auto eprint(std::string_view text) -> void
{
    platform::write_to(platform::Stream::Err, text);
}

template<typename... Args>
auto print_to(platform::Stream stream, std::string_view pattern, Args const&... args) -> void
{
    FormatArg arg_array[] = { FormatArg(args)... };
    print_args(stream, pattern, arg_array, sizeof...(Args));
}

template<typename... Args>
auto print(std::string_view pattern, Args const&... args) -> void
{
    print_to(platform::Stream::Out, pattern, args...);
}

template<typename... Args>
auto eprint(std::string_view pattern, Args const&... args) -> void
{
    print_to(platform::Stream::Err, pattern, args...);
}

} // namespace pup
