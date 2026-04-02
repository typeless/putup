// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstddef>
#include <cstdint>

namespace pup {

class IdBitSet final {
public:
    IdBitSet() = default;
    ~IdBitSet();

    IdBitSet(IdBitSet const&) = delete;
    auto operator=(IdBitSet const&) -> IdBitSet& = delete;

    IdBitSet(IdBitSet&&) noexcept;
    auto operator=(IdBitSet&&) noexcept -> IdBitSet&;

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
    std::uint64_t* words_ = nullptr;
    std::size_t word_count_ = 0;
};

} // namespace pup
