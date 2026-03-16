// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/sorted_id_vec.hpp"

#include <cstdlib>
#include <cstring>
#include <utility>

namespace pup {

// --- SortedIdVec ---

SortedIdVec::~SortedIdVec()
{
    std::free(data_);
}

SortedIdVec::SortedIdVec(SortedIdVec&& other) noexcept
    : data_(std::exchange(other.data_, nullptr))
    , size_(std::exchange(other.size_, 0))
    , capacity_(std::exchange(other.capacity_, 0))
{
}

auto SortedIdVec::operator=(SortedIdVec&& other) noexcept -> SortedIdVec&
{
    if (this != &other) {
        std::free(data_);
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        capacity_ = std::exchange(other.capacity_, 0);
    }
    return *this;
}

auto SortedIdVec::grow() -> void
{
    auto const new_cap = capacity_ == 0 ? std::size_t { 8 } : capacity_ * 2;
    auto* p = static_cast<std::uint32_t*>(std::realloc(data_, new_cap * sizeof(std::uint32_t)));
    if (!p) {
        std::abort();
    }
    data_ = p;
    capacity_ = new_cap;
}

auto SortedIdVec::lower_bound(std::uint32_t id) const -> std::size_t
{
    auto lo = std::size_t { 0 };
    auto hi = size_;
    while (lo < hi) {
        auto const mid = lo + (hi - lo) / 2;
        if (data_[mid] < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

auto SortedIdVec::insert(std::uint32_t id) -> bool
{
    auto const pos = lower_bound(id);
    if (pos < size_ && data_[pos] == id) {
        return false;
    }
    if (size_ == capacity_) {
        grow();
    }
    if (pos < size_) {
        std::memmove(data_ + pos + 1, data_ + pos, (size_ - pos) * sizeof(std::uint32_t));
    }
    data_[pos] = id;
    ++size_;
    return true;
}

auto SortedIdVec::contains(std::uint32_t id) const -> bool
{
    auto const pos = lower_bound(id);
    return pos < size_ && data_[pos] == id;
}

auto SortedIdVec::remove(std::uint32_t id) -> bool
{
    auto const pos = lower_bound(id);
    if (pos >= size_ || data_[pos] != id) {
        return false;
    }
    if (pos + 1 < size_) {
        std::memmove(data_ + pos, data_ + pos + 1, (size_ - pos - 1) * sizeof(std::uint32_t));
    }
    --size_;
    return true;
}

auto SortedIdVec::clear() -> void
{
    size_ = 0;
}

auto SortedIdVec::size() const -> std::size_t
{
    return size_;
}

auto SortedIdVec::data() const -> std::uint32_t const*
{
    return data_;
}

auto SortedIdVec::for_each(void (*fn)(std::uint32_t id, void* ctx), void* ctx) const -> void
{
    for (auto i = std::size_t { 0 }; i < size_; ++i) {
        fn(data_[i], ctx);
    }
}

// --- SortedPairVec ---

SortedPairVec::~SortedPairVec()
{
    std::free(data_);
}

SortedPairVec::SortedPairVec(SortedPairVec&& other) noexcept
    : data_(std::exchange(other.data_, nullptr))
    , size_(std::exchange(other.size_, 0))
    , capacity_(std::exchange(other.capacity_, 0))
{
}

auto SortedPairVec::operator=(SortedPairVec&& other) noexcept -> SortedPairVec&
{
    if (this != &other) {
        std::free(data_);
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        capacity_ = std::exchange(other.capacity_, 0);
    }
    return *this;
}

auto SortedPairVec::grow() -> void
{
    auto const new_cap = capacity_ == 0 ? std::size_t { 8 } : capacity_ * 2;
    auto* p = static_cast<Pair*>(std::realloc(data_, new_cap * sizeof(Pair)));
    if (!p) {
        std::abort();
    }
    data_ = p;
    capacity_ = new_cap;
}

auto SortedPairVec::lower_bound(std::uint32_t key) const -> std::size_t
{
    auto lo = std::size_t { 0 };
    auto hi = size_;
    while (lo < hi) {
        auto const mid = lo + (hi - lo) / 2;
        if (data_[mid].key < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

auto SortedPairVec::insert(std::uint32_t key, std::uint32_t value) -> bool
{
    auto const pos = lower_bound(key);
    if (pos < size_ && data_[pos].key == key) {
        data_[pos].value = value;
        return false;
    }
    if (size_ == capacity_) {
        grow();
    }
    if (pos < size_) {
        std::memmove(data_ + pos + 1, data_ + pos, (size_ - pos) * sizeof(Pair));
    }
    data_[pos] = Pair { key, value };
    ++size_;
    return true;
}

auto SortedPairVec::find(std::uint32_t key) const -> std::uint32_t const*
{
    auto const pos = lower_bound(key);
    if (pos >= size_ || data_[pos].key != key) {
        return nullptr;
    }
    return &data_[pos].value;
}

auto SortedPairVec::contains(std::uint32_t key) const -> bool
{
    auto const pos = lower_bound(key);
    return pos < size_ && data_[pos].key == key;
}

auto SortedPairVec::remove(std::uint32_t key) -> bool
{
    auto const pos = lower_bound(key);
    if (pos >= size_ || data_[pos].key != key) {
        return false;
    }
    if (pos + 1 < size_) {
        std::memmove(data_ + pos, data_ + pos + 1, (size_ - pos - 1) * sizeof(Pair));
    }
    --size_;
    return true;
}

auto SortedPairVec::clear() -> void
{
    size_ = 0;
}

auto SortedPairVec::size() const -> std::size_t
{
    return size_;
}

auto SortedPairVec::for_each(void (*fn)(std::uint32_t key, std::uint32_t value, void* ctx), void* ctx) const -> void
{
    for (auto i = std::size_t { 0 }; i < size_; ++i) {
        fn(data_[i].key, data_[i].value, ctx);
    }
}

} // namespace pup
