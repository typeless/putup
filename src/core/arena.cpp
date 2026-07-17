// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/arena.hpp"

#include <cassert>
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

auto Arena32::append(std::uint32_t const* values, std::uint32_t count) -> ArenaSlice
{
    if (count == 0) {
        return ArenaSlice { static_cast<std::uint32_t>(size_), 0 };
    }
    grow(size_ + count);
    auto const offset = static_cast<std::uint32_t>(size_);
    if (values) {
        std::memcpy(data() + size_, values, count * sizeof(std::uint32_t));
    } else {
        std::memset(data() + size_, 0, count * sizeof(std::uint32_t));
    }
    size_ += count;
    return ArenaSlice { offset, count };
}

auto Arena32::get(ArenaSlice slice) const -> std::uint32_t const*
{
    if (slice.length == 0) {
        return nullptr;
    }
    return data() + slice.offset;
}

auto Arena32::slice(ArenaSlice s) const -> Span
{
    if (s.length == 0) {
        return { nullptr, 0 };
    }
    return { data() + s.offset, s.length };
}

auto Arena32::at(std::uint32_t offset) -> std::uint32_t&
{
    assert(offset < size_);
    return data()[offset];
}

auto Arena32::append_extend(ArenaSlice old, std::uint32_t new_value) -> ArenaSlice
{
    auto new_len = old.length + 1;
    grow(size_ + new_len);
    auto new_offset = static_cast<std::uint32_t>(size_);
    if (old.length > 0) {
        std::memcpy(data() + new_offset, data() + old.offset, old.length * sizeof(std::uint32_t));
    }
    data()[new_offset + old.length] = new_value;
    size_ += new_len;
    return ArenaSlice { new_offset, new_len };
}

auto Arena32::size() const -> std::size_t
{
    return size_;
}

auto Arena32::reserve(std::size_t total_elements) -> void
{
    grow(total_elements);
}

auto Arena32::compact() -> void
{
    region_.shrink(size_ * sizeof(std::uint32_t));
}

auto Arena32::clear() -> void
{
    size_ = 0;
}

} // namespace pup
