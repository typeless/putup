// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/id_bitset.hpp"

#include <cstdlib>
#include <cstring>
#include <utility>

namespace pup {

IdBitSet::~IdBitSet()
{
    std::free(words_);
}

IdBitSet::IdBitSet(IdBitSet&& other) noexcept
    : words_(std::exchange(other.words_, nullptr))
    , word_count_(std::exchange(other.word_count_, 0))
{
}

auto IdBitSet::operator=(IdBitSet&& other) noexcept -> IdBitSet&
{
    if (this != &other) {
        std::free(words_);
        words_ = std::exchange(other.words_, nullptr);
        word_count_ = std::exchange(other.word_count_, 0);
    }
    return *this;
}

auto IdBitSet::resize(std::uint32_t max_id) -> void
{
    auto const needed = static_cast<std::size_t>(max_id / 64) + 1;
    if (needed <= word_count_) {
        return;
    }
    auto* p = static_cast<std::uint64_t*>(std::realloc(words_, needed * sizeof(std::uint64_t)));
    if (!p) {
        return;
    }
    std::memset(p + word_count_, 0, (needed - word_count_) * sizeof(std::uint64_t));
    words_ = p;
    word_count_ = needed;
}

auto IdBitSet::insert(std::uint32_t id) -> void
{
    auto const word = static_cast<std::size_t>(id / 64);
    if (word >= word_count_) {
        resize(id);
    }
    words_[word] |= std::uint64_t { 1 } << (id % 64);
}

auto IdBitSet::remove(std::uint32_t id) -> void
{
    auto const word = static_cast<std::size_t>(id / 64);
    if (word >= word_count_) {
        return;
    }
    words_[word] &= ~(std::uint64_t { 1 } << (id % 64));
}

auto IdBitSet::contains(std::uint32_t id) const -> bool
{
    auto const word = static_cast<std::size_t>(id / 64);
    if (word >= word_count_) {
        return false;
    }
    return (words_[word] & (std::uint64_t { 1 } << (id % 64))) != 0;
}

auto IdBitSet::clear() -> void
{
    if (words_) {
        std::memset(words_, 0, word_count_ * sizeof(std::uint64_t));
    }
}

auto IdBitSet::count() const -> std::size_t
{
    auto n = std::size_t { 0 };
    for (auto i = std::size_t { 0 }; i < word_count_; ++i) {
        n += static_cast<std::size_t>(__builtin_popcountll(words_[i]));
    }
    return n;
}

auto IdBitSet::for_each(void (*fn)(std::uint32_t id, void* ctx), void* ctx) const -> void
{
    for (auto i = std::size_t { 0 }; i < word_count_; ++i) {
        auto w = words_[i];
        while (w != 0) {
            auto const bit = static_cast<std::uint32_t>(__builtin_ctzll(w));
            fn(static_cast<std::uint32_t>(i * 64) + bit, ctx);
            w &= w - 1;
        }
    }
}

} // namespace pup
