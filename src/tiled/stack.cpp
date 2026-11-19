// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#include "tiled/stack.hpp"
#include "tiled/platform.hpp"

#include <cstdint>
#include <string_view>
#include <utility>

namespace tiled {

namespace {

[[noreturn]]
auto fail(std::string_view message) -> void
{
    fatal(message.data(), message.size());
}

auto cursor_for(Tile const& tile) -> StackCursor
{
    auto cursor = StackCursor::make(std::uint64_t { 1 } << tile.size_log2(), page_size());
    if (!cursor) {
        fail("tiled: a stack's tile is smaller than a page");
    }
    return *cursor;
}

} // namespace

Stack::Stack(Tile tile)
    : tile_(std::move(tile))
    , cursor_(cursor_for(tile_))
{
}

auto Stack::push(std::size_t bytes, std::size_t align) -> void*
{
    auto const committed_before = cursor_.committed();
    auto const offset = cursor_.push(bytes, align);
    if (!offset) {
        fail("tiled: stack push refused: the tile is exhausted, or the alignment is not a power of two");
    }
    auto const committed_after = cursor_.committed();
    if (committed_after > committed_before) {
        commit(tile_.base() + committed_before, committed_after - committed_before);
    }
    return tile_.base() + *offset;
}

auto Stack::extend_top(void* p, std::size_t old_bytes, std::size_t new_bytes) -> bool
{
    auto const offset = reinterpret_cast<std::uintptr_t>(p) - reinterpret_cast<std::uintptr_t>(tile_.base());
    auto const committed_before = cursor_.committed();
    if (!cursor_.extend_top(offset, old_bytes, new_bytes)) {
        return false;
    }
    auto const committed_after = cursor_.committed();
    if (committed_after > committed_before) {
        commit(tile_.base() + committed_before, committed_after - committed_before);
    }
    return true;
}

auto Stack::pop_top(void* p, std::size_t bytes) -> void
{
    auto const offset = reinterpret_cast<std::uintptr_t>(p) - reinterpret_cast<std::uintptr_t>(tile_.base());
    cursor_.pop_top(offset, bytes);
}

auto Stack::decommit_above(std::size_t keep) -> void
{
    auto const committed_before = cursor_.committed();
    cursor_.decommit_above(keep);
    auto const committed_after = cursor_.committed();
    if (committed_after < committed_before) {
        decommit(tile_.base() + committed_after, committed_before - committed_after);
    }
}

} // namespace tiled
