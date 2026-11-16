// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#pragma once

#include <cstdint>

namespace tiled {

struct ImageNode {
    std::uint64_t base;
    std::uint8_t log2;
};

struct Layout {
    std::uint8_t root_log2;
    ImageNode image_nodes[2];
    std::uint64_t base;
    std::uint8_t pool_log2;
    std::uint8_t heap_log2;
    std::uint8_t min_tile_log2;
};

consteval auto is_valid(Layout const& layout) -> bool
{
    if (layout.root_log2 > 63 || layout.pool_log2 > layout.root_log2) {
        return false;
    }
    if (layout.min_tile_log2 > layout.heap_log2 || layout.heap_log2 >= layout.pool_log2) {
        return false;
    }
    auto const root_end = std::uint64_t { 1 } << layout.root_log2;
    auto const pool_bytes = std::uint64_t { 1 } << layout.pool_log2;
    if (layout.base % pool_bytes != 0 || layout.base > root_end - pool_bytes) {
        return false;
    }
    for (auto const& node : layout.image_nodes) {
        if (node.log2 > layout.root_log2) {
            return false;
        }
        auto const node_bytes = std::uint64_t { 1 } << node.log2;
        if (node.base % node_bytes != 0 || node.base > root_end - node_bytes) {
            return false;
        }
        auto const disjoint = node.base + node_bytes <= layout.base || layout.base + pool_bytes <= node.base;
        if (!disjoint) {
            return false;
        }
    }
    return true;
}

} // namespace tiled
