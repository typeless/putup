// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/format_to.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pup {

enum class Stream : std::uint8_t { Out,
                                   Err };

auto write_to(Stream stream, std::string_view text) -> void;
auto flush(Stream stream) -> void;
auto print_args(Stream stream, std::string_view pattern, FormatArg const* args, std::size_t count) -> void;

inline auto print(std::string_view text) -> void
{
    write_to(Stream::Out, text);
}

inline auto eprint(std::string_view text) -> void
{
    write_to(Stream::Err, text);
}

template<typename... Args>
auto print_to(Stream stream, std::string_view pattern, Args const&... args) -> void
{
    FormatArg arg_array[] = { FormatArg(args)... };
    print_args(stream, pattern, arg_array, sizeof...(Args));
}

template<typename... Args>
auto print(std::string_view pattern, Args const&... args) -> void
{
    print_to(Stream::Out, pattern, args...);
}

template<typename... Args>
auto eprint(std::string_view pattern, Args const&... args) -> void
{
    print_to(Stream::Err, pattern, args...);
}

} // namespace pup
