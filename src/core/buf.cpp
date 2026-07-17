// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/buf.hpp"
#include "pup/core/format_to.hpp"
#include "pup/core/region.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

namespace pup {

auto Buf::grow(std::size_t needed) -> void
{
    if (needed <= capacity_) {
        return;
    }
    auto new_cap = static_cast<std::size_t>(capacity_) + capacity_ / 2 + 16;
    if (new_cap < needed) {
        new_cap = needed;
    }
    if (!is_spilled()) {
        region_ = Region { new_cap > SPILL_RESERVE ? new_cap : SPILL_RESERVE };
        region_.ensure(new_cap);
        std::memcpy(region_.data(), buf_, size_);
    } else if (new_cap > region_.reserved()) {
        auto bigger = Region { new_cap * 2 };
        bigger.ensure(new_cap);
        std::memcpy(bigger.data(), data_, size_);
        region_ = std::move(bigger);
    } else {
        region_.ensure(new_cap);
    }
    data_ = static_cast<char*>(region_.data());
    capacity_ = static_cast<std::uint32_t>(region_.committed());
}

auto Buf::append(std::string_view sv) -> void
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

auto Buf::append(char c) -> void
{
    grow(size_ + 2);
    data_[size_++] = c;
    data_[size_] = '\0';
}

auto Buf::operator+=(std::string_view sv) -> Buf&
{
    append(sv);
    return *this;
}

auto Buf::operator+=(char c) -> Buf&
{
    append(c);
    return *this;
}

auto Buf::reserve(std::size_t n) -> void
{
    grow(n + 1);
}

auto Buf::clear() -> void
{
    size_ = 0;
    data_[0] = '\0';
}

auto Buf::c_str() const -> char const*
{
    return data_;
}

auto Buf::intern(StringPool& pool) const -> StringId
{
    return pool.intern(view());
}

auto Buf::fmt(std::string_view pattern) -> void
{
    format_to(*this, pattern, nullptr, 0);
}

} // namespace pup
