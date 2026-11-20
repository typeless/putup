// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#pragma once

#include "tiled/pool.hpp"
#include "tiled/stack.hpp"
#include "tiled/tile.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

namespace tiled {

class StackSlot final {
public:
    StackSlot() = default;

    StackSlot(StackSlot const&) = delete;
    auto operator=(StackSlot const&) -> StackSlot& = delete;

    StackSlot(StackSlot&& other) noexcept
    {
        take(other);
    }

    auto operator=(StackSlot&& other) noexcept -> StackSlot&
    {
        if (this != &other) {
            reset();
            take(other);
        }
        return *this;
    }

    ~StackSlot()
    {
        reset();
    }

    auto emplace(Pool& pool, std::uint8_t size_log2) -> Stack&
    {
        reset();
        auto* const stack = new (storage_) Stack { Tile::acquire(pool, size_log2) };
        engaged_ = true;
        return *stack;
    }

    auto reset() -> void
    {
        if (engaged_) {
            held()->~Stack();
            engaged_ = false;
        }
    }

    [[nodiscard]]
    explicit operator bool() const
    {
        return engaged_;
    }

    [[nodiscard]]
    auto operator->() -> Stack*
    {
        return held();
    }

    [[nodiscard]]
    auto operator->() const -> Stack const*
    {
        return std::launder(reinterpret_cast<Stack const*>(storage_));
    }

private:
    [[nodiscard]]
    auto held() -> Stack*
    {
        return std::launder(reinterpret_cast<Stack*>(storage_));
    }

    auto take(StackSlot& other) noexcept -> void
    {
        if (!other.engaged_) {
            return;
        }
        new (storage_) Stack { std::move(*other.held()) };
        other.reset();
        engaged_ = true;
    }

    alignas(Stack) std::byte storage_[sizeof(Stack)];
    bool engaged_ = false;
};

} // namespace tiled
