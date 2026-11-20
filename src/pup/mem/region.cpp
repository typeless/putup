// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/mem/region.hpp"
#include "pup/mem/bump_alloc.hpp"
#include "pup/mem/memory_layout.hpp"
#include "pup/platform/stream.hpp"

#include <bit>
#include <charconv>
#include <cstdint>
#include <cstdlib>

namespace pup::mem {

namespace {

auto write_uint(std::size_t n) -> void
{
    char buf[20];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), n);
    platform::write_to(platform::Stream::Err, { buf, static_cast<std::size_t>(end - buf) });
}

auto tile_log2_for(std::size_t reserve_bytes) -> std::uint8_t
{
    auto const wanted = static_cast<std::uint8_t>(std::bit_width(reserve_bytes - 1));
    return wanted > memory_layout.min_tile_log2 ? wanted : memory_layout.min_tile_log2;
}

} // namespace

Region::Region(std::size_t reserve_bytes)
    : reserve_bytes_(reserve_bytes)
{
}

auto Region::ensure(std::size_t bytes) -> void
{
    if (bytes <= committed()) {
        return;
    }
    if (!stack_) {
        stack_.emplace(process_pool(), tile_log2_for(reserve_bytes_));
    }
    if (bytes > stack_->reserved()) {
        platform::write_to(platform::Stream::Err, "fatal: region reservation exhausted (");
        write_uint(bytes);
        platform::write_to(platform::Stream::Err, " > ");
        write_uint(stack_->reserved());
        platform::write_to(platform::Stream::Err, " bytes)\n");
        std::abort();
    }
    auto const top = stack_->top();
    if (top == 0) {
        (void)stack_->push(bytes, 1);
    } else if (bytes > top) {
        (void)stack_->extend_top(stack_->base(), top, bytes);
    }
}

auto Region::shrink(std::size_t keep_bytes) -> void
{
    if (keep_bytes == 0) {
        stack_.reset();
        return;
    }
    if (!stack_) {
        return;
    }
    auto const top = stack_->top();
    if (keep_bytes < top) {
        (void)stack_->extend_top(stack_->base(), top, keep_bytes);
    }
    stack_->decommit_above(keep_bytes);
}

} // namespace pup::mem
