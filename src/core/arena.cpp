// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/arena.hpp"

#include <cstdint>
#include <cstring>
#include <utility>

namespace pup {

Arena32::Arena32(Arena32&& other) noexcept
    : region_(std::move(other.region_))
    , size_(std::exchange(other.size_, 0))
{
}

auto Arena32::operator=(Arena32&& other) noexcept -> Arena32&
{
    if (this != &other) {
        region_ = std::move(other.region_);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

auto Arena32::data() const -> std::uint32_t*
{
    return static_cast<std::uint32_t*>(region_.data());
}

auto Arena32::grow(std::size_t needed) -> void
{
    region_.ensure(needed * sizeof(std::uint32_t));
}

auto Arena32::slice(ArenaSlice s) const -> Span
{
    if (s.length == 0) {
        return { nullptr, 0 };
    }
    return { data() + s.offset, s.length };
}

// Every slice is born here and blocks are sized to the next power of two
// of the slice length, so a non-power-of-two length proves a spare slot
// remains at the end of the block.
auto Arena32::append_extend(ArenaSlice old, std::uint32_t new_value) -> ArenaSlice
{
    if (old.length > 0 && (old.length & (old.length - 1)) != 0) {
        data()[old.offset + old.length] = new_value;
        return ArenaSlice { old.offset, old.length + 1 };
    }
    auto const block = old.length == 0 ? 1 : old.length * 2;
    grow(size_ + block);
    auto const new_offset = static_cast<std::uint32_t>(size_);
    if (old.length > 0) {
        std::memcpy(data() + new_offset, data() + old.offset, old.length * sizeof(std::uint32_t));
    }
    data()[new_offset + old.length] = new_value;
    size_ += block;
    return ArenaSlice { new_offset, old.length + 1 };
}

auto Arena32::size() const -> std::size_t
{
    return size_;
}

auto Arena32::clear() -> void
{
    size_ = 0;
}

} // namespace pup
