// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/region.hpp"

#include <cstddef>
#include <cstdint>

namespace pup {

struct ArenaSlice {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

class Arena32 final {
public:
    Arena32() = default;
    ~Arena32() = default;

    Arena32(Arena32 const&) = delete;
    auto operator=(Arena32 const&) -> Arena32& = delete;

    Arena32(Arena32&&) noexcept;
    auto operator=(Arena32&&) noexcept -> Arena32&;

    struct Span {
        std::uint32_t const* data;
        std::uint32_t length;
        auto begin() const -> std::uint32_t const* { return data; }
        auto end() const -> std::uint32_t const* { return data + length; }
        auto operator[](std::uint32_t i) const -> std::uint32_t { return data[i]; }
        [[nodiscard]]
        auto size() const -> std::uint32_t
        {
            return length;
        }
        [[nodiscard]]
        auto empty() const -> bool
        {
            return length == 0;
        }
    };

    [[nodiscard]]
    auto slice(ArenaSlice s) const -> Span;

    [[nodiscard]]
    auto size() const -> std::size_t;

    auto append_extend(ArenaSlice old, std::uint32_t new_value) -> ArenaSlice;

    auto clear() -> void;

private:
    Region region_;
    std::size_t size_ = 0;

    [[nodiscard]]
    auto data() const -> std::uint32_t*;
    auto grow(std::size_t needed) -> void;
};

} // namespace pup
