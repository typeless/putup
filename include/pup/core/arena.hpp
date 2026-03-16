// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstddef>
#include <cstdint>

namespace pup {

struct ArenaSlice {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

class Arena32 {
public:
    Arena32() = default;
    ~Arena32();

    Arena32(Arena32 const&) = delete;
    auto operator=(Arena32 const&) -> Arena32& = delete;

    Arena32(Arena32&&) noexcept;
    auto operator=(Arena32&&) noexcept -> Arena32&;

    auto append(std::uint32_t const* values, std::uint32_t count) -> ArenaSlice;

    [[nodiscard]]
    auto get(ArenaSlice slice) const -> std::uint32_t const*;

    [[nodiscard]]
    auto size() const -> std::size_t;

    auto reserve(std::size_t total_elements) -> void;
    auto compact() -> void;
    auto clear() -> void;

private:
    std::uint32_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;

    auto grow(std::size_t needed) -> void;
};

} // namespace pup
