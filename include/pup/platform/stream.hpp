// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstdint>
#include <string_view>

namespace pup::platform {

enum class Stream : std::uint8_t { Out,
                                   Err };

auto write_to(Stream stream, std::string_view text) -> void;
auto flush(Stream stream) -> void;

} // namespace pup::platform
