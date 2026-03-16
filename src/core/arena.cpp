// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/arena.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace pup {

Arena32::~Arena32()
{
    std::free(data_);
}

Arena32::Arena32(Arena32&& other) noexcept
    : data_(std::exchange(other.data_, nullptr))
    , size_(std::exchange(other.size_, 0))
    , capacity_(std::exchange(other.capacity_, 0))
{
}

auto Arena32::operator=(Arena32&& other) noexcept -> Arena32&
{
    if (this != &other) {
        std::free(data_);
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        capacity_ = std::exchange(other.capacity_, 0);
    }
    return *this;
}

auto Arena32::grow(std::size_t needed) -> void
{
    if (needed <= capacity_) {
        return;
    }
    auto new_cap = capacity_ == 0 ? std::size_t { 16 } : capacity_ * 2;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    auto* p = static_cast<std::uint32_t*>(std::realloc(data_, new_cap * sizeof(std::uint32_t)));
    if (!p) {
        std::abort();
    }
    data_ = p;
    capacity_ = new_cap;
}

auto Arena32::append(std::uint32_t const* values, std::uint32_t count) -> ArenaSlice
{
    if (count == 0) {
        return ArenaSlice { static_cast<std::uint32_t>(size_), 0 };
    }
    auto const needed = size_ + count;
    if (needed > capacity_) {
        grow(needed);
    }
    auto const offset = static_cast<std::uint32_t>(size_);
    if (values) {
        std::memcpy(data_ + size_, values, count * sizeof(std::uint32_t));
    } else {
        std::memset(data_ + size_, 0, count * sizeof(std::uint32_t));
    }
    size_ += count;
    return ArenaSlice { offset, count };
}

auto Arena32::get(ArenaSlice slice) const -> std::uint32_t const*
{
    if (slice.length == 0) {
        return nullptr;
    }
    return data_ + slice.offset;
}

auto Arena32::slice(ArenaSlice s) const -> Span
{
    if (s.length == 0) {
        return { nullptr, 0 };
    }
    return { data_ + s.offset, s.length };
}

auto Arena32::at(std::uint32_t offset) -> std::uint32_t&
{
    assert(offset < size_);
    return data_[offset];
}

auto Arena32::append_extend(ArenaSlice old, std::uint32_t new_value) -> ArenaSlice
{
    auto new_len = old.length + 1;
    auto needed = size_ + new_len;
    if (needed > capacity_) {
        grow(needed);
    }
    auto new_offset = static_cast<std::uint32_t>(size_);
    if (old.length > 0) {
        std::memcpy(data_ + new_offset, data_ + old.offset, old.length * sizeof(std::uint32_t));
    }
    data_[new_offset + old.length] = new_value;
    size_ += new_len;
    return ArenaSlice { new_offset, new_len };
}

auto Arena32::size() const -> std::size_t
{
    return size_;
}

auto Arena32::reserve(std::size_t total_elements) -> void
{
    if (total_elements > capacity_) {
        grow(total_elements);
    }
}

auto Arena32::compact() -> void
{
    if (size_ == capacity_ || size_ == 0) {
        if (size_ == 0 && data_) {
            std::free(data_);
            data_ = nullptr;
            capacity_ = 0;
        }
        return;
    }
    auto* p = static_cast<std::uint32_t*>(std::realloc(data_, size_ * sizeof(std::uint32_t)));
    if (p) {
        data_ = p;
        capacity_ = size_;
    }
}

auto Arena32::clear() -> void
{
    size_ = 0;
}

} // namespace pup
