// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/heap_buf.hpp"
#include "pup/core/format_to.hpp"
#include "pup/core/region.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

namespace pup {

HeapBuf::~HeapBuf() = default;

HeapBuf::HeapBuf(HeapBuf&& other) noexcept
    : region_(std::move(other.region_))
    , size_(std::exchange(other.size_, 0))
{
}

auto HeapBuf::operator=(HeapBuf&& other) noexcept -> HeapBuf&
{
    if (this != &other) {
        region_ = std::move(other.region_);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

auto HeapBuf::grow(std::size_t needed) -> void
{
    if (needed <= region_.committed()) {
        return;
    }
    auto const cap = region_.committed();
    auto new_cap = cap + cap / 2 + 16;
    if (new_cap < needed) {
        new_cap = needed;
    }
    if (!region_.data()) {
        region_ = Region { new_cap > SPILL_RESERVE ? new_cap : SPILL_RESERVE };
    } else if (new_cap > region_.reserved()) {
        auto bigger = Region { new_cap * 2 };
        bigger.ensure(new_cap);
        std::memcpy(bigger.data(), region_.data(), size_);
        region_ = std::move(bigger);
    }
    region_.ensure(new_cap);
}

auto HeapBuf::append(std::string_view sv) -> void
{
    if (sv.empty()) {
        return;
    }
    auto new_size = size_ + sv.size();
    grow(new_size + 1);
    std::memcpy(data() + size_, sv.data(), sv.size());
    size_ = static_cast<std::uint32_t>(new_size);
    data()[size_] = '\0';
}

auto HeapBuf::append(char c) -> void
{
    grow(size_ + 2);
    data()[size_++] = c;
    data()[size_] = '\0';
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
        std::memset(data() + size_, 0, n - size_);
    }
    size_ = static_cast<std::uint32_t>(n);
    data()[size_] = '\0';
}

auto HeapBuf::clear() -> void
{
    size_ = 0;
    if (data()) {
        data()[0] = '\0';
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
