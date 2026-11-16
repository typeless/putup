// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#pragma once

#include "tiled/layout.hpp"

#include <cstdint>

namespace pup {

#if defined(_WIN32)
inline constexpr auto memory_layout = tiled::Layout {
    .root_log2 = 47,
    .image_nodes = { { 0, 33 }, { std::uint64_t { 1 } << 46, 46 } },
    .base = std::uint64_t { 1 } << 45,
    .pool_log2 = 44,
    .heap_log2 = 34,
    .min_tile_log2 = 24,
};
#else
inline constexpr auto memory_layout = tiled::Layout {
    .root_log2 = 47,
    .image_nodes = { { 0, 33 }, { std::uint64_t { 1 } << 46, 46 } },
    .base = std::uint64_t { 1 } << 45,
    .pool_log2 = 44,
    .heap_log2 = 34,
    .min_tile_log2 = 24,
};
#endif

} // namespace pup
