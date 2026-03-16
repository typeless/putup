// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/id_bitset.hpp"

#include <cstddef>
#include <cstdint>

namespace pup {

class IdArray32 {
public:
    IdArray32() = default;
    ~IdArray32();

    IdArray32(IdArray32 const&) = delete;
    auto operator=(IdArray32 const&) -> IdArray32& = delete;

    IdArray32(IdArray32&&) noexcept;
    auto operator=(IdArray32&&) noexcept -> IdArray32&;

    auto resize(std::uint32_t max_id) -> void;
    auto set(std::uint32_t id, std::uint32_t value) -> void;

    [[nodiscard]]
    auto get(std::uint32_t id) const -> std::uint32_t;

    [[nodiscard]]
    auto contains(std::uint32_t id) const -> bool;

    auto remove(std::uint32_t id) -> void;
    auto clear() -> void;

    [[nodiscard]]
    auto count() const -> std::size_t { return present_.count(); }

    auto for_each(void (*fn)(std::uint32_t id, std::uint32_t value, void* ctx), void* ctx) const -> void;

private:
    std::uint32_t* data_ = nullptr;
    std::size_t capacity_ = 0;
    IdBitSet present_;
};

} // namespace pup
