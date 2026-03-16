// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/arena.hpp"

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
        return;
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
    std::memcpy(data_ + size_, values, count * sizeof(std::uint32_t));
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
