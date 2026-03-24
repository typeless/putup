// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstddef>
#include <cstdint>

namespace pup {

class SortedIdVec {
public:
    SortedIdVec() = default;
    ~SortedIdVec();

    SortedIdVec(SortedIdVec const&) = delete;
    auto operator=(SortedIdVec const&) -> SortedIdVec& = delete;

    SortedIdVec(SortedIdVec&&) noexcept;
    auto operator=(SortedIdVec&&) noexcept -> SortedIdVec&;

    auto insert(std::uint32_t id) -> bool;

    [[nodiscard]]
    auto contains(std::uint32_t id) const -> bool;

    auto remove(std::uint32_t id) -> bool;
    auto clear() -> void;

    [[nodiscard]]
    auto empty() const -> bool
    {
        return size_ == 0;
    }

    [[nodiscard]]
    auto size() const -> std::size_t;

    auto merge_from(SortedIdVec const& other) -> void;

    [[nodiscard]]
    auto data() const -> std::uint32_t const*;

    [[nodiscard]]
    auto begin() const -> std::uint32_t const*
    {
        return data_;
    }

    [[nodiscard]]
    auto end() const -> std::uint32_t const*
    {
        return data_ + size_;
    }

    auto for_each(void (*fn)(std::uint32_t id, void* ctx), void* ctx) const -> void;

private:
    std::uint32_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;

    auto grow() -> void;
    [[nodiscard]]
    auto lower_bound(std::uint32_t id) const -> std::size_t;
};

class SortedPairVec {
public:
    SortedPairVec() = default;
    ~SortedPairVec();

    SortedPairVec(SortedPairVec const&) = delete;
    auto operator=(SortedPairVec const&) -> SortedPairVec& = delete;

    SortedPairVec(SortedPairVec&&) noexcept;
    auto operator=(SortedPairVec&&) noexcept -> SortedPairVec&;

    auto insert(std::uint32_t key, std::uint32_t value) -> bool;

    [[nodiscard]]
    auto find(std::uint32_t key) const -> std::uint32_t const*;

    [[nodiscard]]
    auto contains(std::uint32_t key) const -> bool;

    auto remove(std::uint32_t key) -> bool;
    auto clear() -> void;

    [[nodiscard]]
    auto empty() const -> bool
    {
        return size_ == 0;
    }

    [[nodiscard]]
    auto size() const -> std::size_t;

    auto for_each(void (*fn)(std::uint32_t key, std::uint32_t value, void* ctx), void* ctx) const -> void;

    struct Pair {
        std::uint32_t key;
        std::uint32_t value;
    };

    [[nodiscard]]
    auto begin() const -> Pair const*
    {
        return data_;
    }

    [[nodiscard]]
    auto end() const -> Pair const*
    {
        return data_ + size_;
    }

private:
    Pair* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;

    auto grow() -> void;
    [[nodiscard]]
    auto lower_bound(std::uint32_t key) const -> std::size_t;
};

} // namespace pup
