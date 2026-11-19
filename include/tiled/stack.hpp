// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#pragma once

#include "tiled/stack_cursor.hpp"
#include "tiled/tile.hpp"

#include <cstddef>

namespace tiled {

class Stack final {
public:
    explicit Stack(Tile tile);

    [[nodiscard]]
    auto push(std::size_t bytes, std::size_t align) -> void*;

    [[nodiscard]]
    auto extend_top(void* p, std::size_t old_bytes, std::size_t new_bytes) -> bool;

    auto pop_top(void* p, std::size_t bytes) -> void;

    auto decommit_above(std::size_t keep) -> void;

    [[nodiscard]]
    auto base() const -> void*
    {
        return tile_.base();
    }

    [[nodiscard]]
    auto top() const -> std::size_t
    {
        return cursor_.top();
    }

    [[nodiscard]]
    auto committed() const -> std::size_t
    {
        return cursor_.committed();
    }

    [[nodiscard]]
    auto reserved() const -> std::size_t
    {
        return std::size_t { 1 } << tile_.size_log2();
    }

private:
    Tile tile_;
    StackCursor cursor_;
};

} // namespace tiled
