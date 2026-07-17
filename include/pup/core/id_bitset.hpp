// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/region.hpp"

#include <cstddef>
#include <cstdint>

namespace pup {

class IdBitSet final {
public:
    IdBitSet() = default;
    ~IdBitSet() = default;

    IdBitSet(IdBitSet const&) = delete;
    auto operator=(IdBitSet const&) -> IdBitSet& = delete;

    IdBitSet(IdBitSet&&) noexcept = default;
    auto operator=(IdBitSet&&) noexcept -> IdBitSet& = default;

    auto resize(std::uint32_t max_id) -> void;
    auto insert(std::uint32_t id) -> void;
    auto remove(std::uint32_t id) -> void;

    [[nodiscard]]
    auto contains(std::uint32_t id) const -> bool;

    auto clear() -> void;

    [[nodiscard]]
    auto count() const -> std::size_t;

    auto for_each(void (*fn)(std::uint32_t id, void* ctx), void* ctx) const -> void;

private:
    // 32-bit ids bound the words at 512 MiB, so the reservation is the
    // exact ceiling and growth can never exhaust it.
    static constexpr auto WORDS_CEILING = (std::size_t { 1 } << 32) / 8;

    Region region_ { WORDS_CEILING };

    [[nodiscard]]
    auto words() const -> std::uint64_t*;

    [[nodiscard]]
    auto word_count() const -> std::size_t;
};

} // namespace pup
