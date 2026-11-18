// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/id_bitset.hpp"

#include <cstdint>
#include <cstring>

namespace pup {

auto IdBitSet::words() const -> std::uint64_t*
{
    return static_cast<std::uint64_t*>(region_.data());
}

auto IdBitSet::word_count() const -> std::size_t
{
    return region_.committed() / sizeof(std::uint64_t);
}

auto IdBitSet::resize(std::uint32_t max_id) -> void
{
    auto const needed = static_cast<std::size_t>(max_id / 64) + 1;
    region_.ensure(needed * sizeof(std::uint64_t));
}

auto IdBitSet::insert(std::uint32_t id) -> void
{
    auto const word = static_cast<std::size_t>(id / 64);
    if (word >= word_count()) {
        resize(id);
    }
    words()[word] |= std::uint64_t { 1 } << (id % 64);
}

auto IdBitSet::remove(std::uint32_t id) -> void
{
    auto const word = static_cast<std::size_t>(id / 64);
    if (word >= word_count()) {
        return;
    }
    words()[word] &= ~(std::uint64_t { 1 } << (id % 64));
}

auto IdBitSet::contains(std::uint32_t id) const -> bool
{
    auto const word = static_cast<std::size_t>(id / 64);
    if (word >= word_count()) {
        return false;
    }
    return (words()[word] & (std::uint64_t { 1 } << (id % 64))) != 0;
}

auto IdBitSet::clear() -> void
{
    if (words()) {
        std::memset(words(), 0, region_.committed());
    }
}

auto IdBitSet::count() const -> std::size_t
{
    auto n = std::size_t { 0 };
    for (auto i = std::size_t { 0 }; i < word_count(); ++i) {
        n += static_cast<std::size_t>(__builtin_popcountll(words()[i]));
    }
    return n;
}

auto IdBitSet::for_each(void (*fn)(std::uint32_t id, void* ctx), void* ctx) const -> void
{
    for (auto i = std::size_t { 0 }; i < word_count(); ++i) {
        auto w = words()[i];
        while (w != 0) {
            auto const bit = static_cast<std::uint32_t>(__builtin_ctzll(w));
            fn(static_cast<std::uint32_t>(i * 64) + bit, ctx);
            w &= w - 1;
        }
    }
}

} // namespace pup
