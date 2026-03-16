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

    struct Span {
        std::uint32_t const* data;
        std::uint32_t length;
        auto begin() const -> std::uint32_t const* { return data; }
        auto end() const -> std::uint32_t const* { return data + length; }
        auto operator[](std::uint32_t i) const -> std::uint32_t { return data[i]; }
        [[nodiscard]] auto size() const -> std::uint32_t { return length; }
        [[nodiscard]] auto empty() const -> bool { return length == 0; }
    };

    [[nodiscard]]
    auto slice(ArenaSlice s) const -> Span;

    [[nodiscard]]
    auto at(std::uint32_t offset) -> std::uint32_t&;

    [[nodiscard]]
    auto size() const -> std::size_t;

    auto append_extend(ArenaSlice old, std::uint32_t new_value) -> ArenaSlice;

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
