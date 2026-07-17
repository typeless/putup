// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/id_bitset.hpp"
#include "pup/core/region.hpp"

#include <cstddef>
#include <cstdint>

namespace pup {

class IdArray32 final {
public:
    IdArray32() = default;
    ~IdArray32() = default;

    IdArray32(IdArray32 const&) = delete;
    auto operator=(IdArray32 const&) -> IdArray32& = delete;

    IdArray32(IdArray32&&) noexcept = default;
    auto operator=(IdArray32&&) noexcept -> IdArray32& = default;

    auto resize(std::uint32_t max_id) -> void;
    auto set(std::uint32_t id, std::uint32_t value) -> void;

    [[nodiscard]]
    auto get(std::uint32_t id) const -> std::uint32_t;

    [[nodiscard]]
    auto contains(std::uint32_t id) const -> bool;

    auto remove(std::uint32_t id) -> void;
    auto clear() -> void;

    [[nodiscard]]
    auto count() const -> std::size_t
    {
        return present_.count();
    }

    auto for_each(void (*fn)(std::uint32_t id, std::uint32_t value, void* ctx), void* ctx) const -> void;

private:
    Region region_;
    IdBitSet present_;

    [[nodiscard]]
    auto data() const -> std::uint32_t*;

    [[nodiscard]]
    auto capacity() const -> std::size_t;
};

} // namespace pup
