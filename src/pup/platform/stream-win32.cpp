// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/stream.hpp"

#include <cstdio>
#include <string_view>

namespace pup::platform {

auto write_to(Stream stream, std::string_view text) -> void
{
    std::fwrite(text.data(), 1, text.size(), stream == Stream::Err ? stderr : stdout);
}

auto flush(Stream stream) -> void
{
    std::fflush(stream == Stream::Err ? stderr : stdout);
}

} // namespace pup::platform
