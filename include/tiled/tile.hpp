// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#pragma once

#include "tiled/pool.hpp"

#include <cstddef>
#include <cstdint>

namespace tiled {

class Tile final {
public:
    [[nodiscard]]
    static auto acquire(Pool& pool, std::uint8_t size_log2) -> Tile;

    Tile(Tile const&) = delete;
    auto operator=(Tile const&) -> Tile& = delete;
    Tile(Tile&& other) noexcept;
    auto operator=(Tile&&) -> Tile& = delete;
    ~Tile();

    [[nodiscard]]
    auto base() const -> std::byte*;

    [[nodiscard]]
    auto size_log2() const -> std::uint8_t;

private:
    Tile(Pool* pool, std::uint64_t offset, std::uint8_t size_log2);

    Pool* pool_;
    std::uint64_t offset_;
    std::uint8_t size_log2_;
};

} // namespace tiled
