// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/heap_buf.hpp"
#include "pup/core/string_pool.hpp"

#include <cstdlib>
#include <cstring>

namespace pup {

HeapBuf::~HeapBuf()
{
    std::free(data_);
}

HeapBuf::HeapBuf(HeapBuf&& other) noexcept
    : data_(other.data_)
    , size_(other.size_)
    , capacity_(other.capacity_)
{
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
}

auto HeapBuf::operator=(HeapBuf&& other) noexcept -> HeapBuf&
{
    if (this != &other) {
        std::free(data_);
        data_ = other.data_;
        size_ = other.size_;
        capacity_ = other.capacity_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }
    return *this;
}

auto HeapBuf::grow(std::size_t needed) -> void
{
    if (needed <= capacity_) {
        return;
    }
    auto new_cap = static_cast<std::size_t>(capacity_) + capacity_ / 2 + 16;
    if (new_cap < needed) {
        new_cap = needed;
    }
    auto* p = static_cast<char*>(std::realloc(data_, new_cap));
    if (!p) {
        std::abort();
    }
    data_ = p;
    capacity_ = static_cast<std::uint32_t>(new_cap);
}

auto HeapBuf::append(std::string_view sv) -> void
{
    if (sv.empty()) {
        return;
    }
    auto new_size = size_ + sv.size();
    grow(new_size + 1);
    std::memcpy(data_ + size_, sv.data(), sv.size());
    size_ = static_cast<std::uint32_t>(new_size);
    data_[size_] = '\0';
}

auto HeapBuf::append(char c) -> void
{
    grow(size_ + 2);
    data_[size_++] = c;
    data_[size_] = '\0';
}

auto HeapBuf::operator+=(std::string_view sv) -> HeapBuf&
{
    append(sv);
    return *this;
}

auto HeapBuf::operator+=(char c) -> HeapBuf&
{
    append(c);
    return *this;
}

auto HeapBuf::reserve(std::size_t n) -> void
{
    grow(n + 1);
}

auto HeapBuf::resize(std::size_t n) -> void
{
    grow(n + 1);
    if (n > size_) {
        std::memset(data_ + size_, 0, n - size_);
    }
    size_ = static_cast<std::uint32_t>(n);
    data_[size_] = '\0';
}

auto HeapBuf::clear() -> void
{
    size_ = 0;
    if (data_) {
        data_[0] = '\0';
    }
}

auto HeapBuf::intern(StringPool& pool) const -> StringId
{
    return pool.intern(view());
}

auto HeapBuf::fmt(std::string_view pattern) -> void
{
    format_to(*this, pattern, nullptr, 0);
}

} // namespace pup
