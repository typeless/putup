// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#include "tiled/tile.hpp"
#include "tiled/platform.hpp"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <utility>

namespace tiled {

namespace {

[[noreturn]]
auto fail(char const* what, std::uint8_t size_log2) -> void
{
    char message[96];
    auto const what_length = std::strlen(what);
    std::memcpy(message, what, what_length);
    auto* end = message + what_length;
    end = std::to_chars(end, message + sizeof(message), size_log2).ptr;
    fatal(message, static_cast<std::size_t>(end - message));
}

} // namespace

Tile::Tile(Pool* pool, std::uint64_t offset, std::uint8_t size_log2)
    : pool_(pool)
    , base_(reinterpret_cast<std::byte*>(pool->base_ + offset))
    , size_log2_(size_log2)
{
}

Tile::Tile(Tile&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr))
    , base_(other.base_)
    , size_log2_(other.size_log2_)
{
}

Tile::~Tile()
{
    if (pool_ == nullptr) {
        return;
    }
    decommit(base_, std::size_t { 1 } << size_log2_);
    if (!pool_->release(reinterpret_cast<std::uintptr_t>(base_) - pool_->base_, size_log2_)) {
        fail("tiled: the pool refused the release of a tile of 2^", size_log2_);
    }
}

auto Tile::acquire(Pool& pool, std::uint8_t size_log2) -> Tile
{
    auto const offset = pool.acquire(size_log2);
    if (!offset) {
        fail("tiled: the pool has no free tile of 2^", size_log2);
    }
    return Tile { &pool, *offset, size_log2 };
}

} // namespace tiled
