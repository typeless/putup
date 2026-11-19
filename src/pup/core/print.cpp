// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/print.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/format_to.hpp"
#include "pup/platform/stream.hpp"

#include <cstddef>
#include <string_view>

namespace pup {

auto print_args(platform::Stream stream, std::string_view pattern, FormatArg const* args, std::size_t count) -> void
{
    auto buf = Buf {};
    format_to(buf, pattern, args, count);
    platform::write_to(stream, buf.view());
}

} // namespace pup
