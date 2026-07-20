// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/print.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/format_to.hpp"

#include <cstddef>
#include <cstdio>
#include <string_view>

namespace pup {

auto write_to(Stream stream, std::string_view text) -> void
{
    std::fwrite(text.data(), 1, text.size(), stream == Stream::Err ? stderr : stdout);
}

auto flush(Stream stream) -> void
{
    std::fflush(stream == Stream::Err ? stderr : stdout);
}

auto print_args(Stream stream, std::string_view pattern, FormatArg const* args, std::size_t count) -> void
{
    auto buf = Buf {};
    format_to(buf, pattern, args, count);
    write_to(stream, buf.view());
}

} // namespace pup
