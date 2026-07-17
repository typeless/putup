// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/region.hpp"

#include <cstddef>
#include <utility>

namespace pup {

/// Append-only growable array with pointer stability AND contiguity:
/// elements live in a Region whose base never moves, so growth neither
/// copies nor invalidates. Replaces PagedVec (and before it, std::deque).
template<typename T>
class StableVec final {
public:
    StableVec() = default;

    ~StableVec()
    {
        clear();
    }

    StableVec(StableVec const&) = delete;
    auto operator=(StableVec const&) -> StableVec& = delete;

    StableVec(StableVec&& other) noexcept
        : region_(std::move(other.region_))
        , size_(std::exchange(other.size_, 0))
    {
    }

    auto operator=(StableVec&& other) noexcept -> StableVec&
    {
        if (this != &other) {
            clear();
            region_ = std::move(other.region_);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    auto push_back(T const& value) -> void
    {
        emplace_back(value);
    }

    auto push_back(T&& value) -> void
    {
        emplace_back(std::move(value));
    }

    template<typename... Args>
    auto emplace_back(Args&&... args) -> T&
    {
        region_.ensure((size_ + 1) * sizeof(T));
        auto* p = new (data() + size_) T(std::forward<Args>(args)...);
        ++size_;
        return *p;
    }

    [[nodiscard]]
    auto operator[](std::size_t i) -> T&
    {
        return data()[i];
    }

    [[nodiscard]]
    auto operator[](std::size_t i) const -> T const&
    {
        return data()[i];
    }

    [[nodiscard]]
    auto back() -> T&
    {
        return data()[size_ - 1];
    }

    [[nodiscard]]
    auto back() const -> T const&
    {
        return data()[size_ - 1];
    }

    [[nodiscard]]
    auto size() const -> std::size_t
    {
        return size_;
    }

    [[nodiscard]]
    auto empty() const -> bool
    {
        return size_ == 0;
    }

    auto resize(std::size_t n) -> void
    {
        while (size_ < n) {
            emplace_back();
        }
    }

    auto clear() -> void
    {
        for (std::size_t i = 0; i < size_; ++i) {
            data()[i].~T();
        }
        size_ = 0;
    }

    auto begin() -> T* { return data(); }
    auto end() -> T* { return data() + size_; }
    auto begin() const -> T const* { return data(); }
    auto end() const -> T const* { return data() + size_; }

private:
    Region region_;
    std::size_t size_ = 0;

    [[nodiscard]]
    auto data() -> T*
    {
        return static_cast<T*>(region_.data());
    }

    [[nodiscard]]
    auto data() const -> T const*
    {
        return static_cast<T const*>(region_.data());
    }
};

} // namespace pup
