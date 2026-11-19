// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#include "tiled/stack_cursor.hpp"

namespace tiled {

namespace {

auto round_up_to_page(std::uint64_t end, std::uint64_t page) -> std::uint64_t
{
    if (end == 0) {
        return 0;
    }
    return ((end - 1) / page + 1) * page;
}

} // namespace

StackCursor::StackCursor(std::uint64_t reserved, std::uint64_t page)
    : reserved_(reserved)
    , page_(page)
{
}

auto StackCursor::make(std::uint64_t reserved, std::uint64_t page) -> std::optional<StackCursor>
{
    if (page == 0 || (page & (page - 1)) != 0 || reserved % page != 0) {
        return std::nullopt;
    }
    return StackCursor { reserved, page };
}

auto StackCursor::push(std::uint64_t bytes, std::uint64_t align) -> std::optional<std::uint64_t>
{
    if (align == 0 || (align & (align - 1)) != 0) {
        return std::nullopt;
    }
    auto const misalignment = top_ & (align - 1);
    auto const padding = misalignment == 0 ? 0 : align - misalignment;
    if (padding > reserved_ - top_) {
        return std::nullopt;
    }
    auto const offset = top_ + padding;
    if (bytes > reserved_ - offset) {
        return std::nullopt;
    }
    top_ = offset + bytes;
    raise_commit_to(top_);
    return offset;
}

auto StackCursor::extend_top(std::uint64_t offset, std::uint64_t old_bytes, std::uint64_t new_bytes) -> bool
{
    if (offset > top_ || top_ - offset != old_bytes || new_bytes > reserved_ - offset) {
        return false;
    }
    top_ = offset + new_bytes;
    raise_commit_to(top_);
    return true;
}

auto StackCursor::pop_top(std::uint64_t offset, std::uint64_t bytes) -> void
{
    if (offset <= top_ && top_ - offset == bytes) {
        top_ = offset;
    }
}

auto StackCursor::decommit_above(std::uint64_t keep) -> void
{
    if (keep >= committed_) {
        return;
    }
    auto const floor = round_up_to_page(keep > top_ ? keep : top_, page_);
    if (floor < committed_) {
        committed_ = floor;
    }
}

auto StackCursor::raise_commit_to(std::uint64_t end) -> void
{
    auto const needed = round_up_to_page(end, page_);
    if (needed > committed_) {
        committed_ = needed;
    }
}

} // namespace tiled
