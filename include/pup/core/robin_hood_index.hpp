// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/region.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace pup {

/// Robin Hood hash index mapping caller-computed hashes to u32 values.
/// The index stores no keys: the caller derives the key from a candidate
/// value (values are ids into caller-owned storage) and verifies matches
/// through the find predicate. One region per table generation holds
/// [Meta x cap][value x cap]; fresh pages read as zero, which is the
/// empty-slot sentinel.
class RobinHoodIndex final {
public:
    RobinHoodIndex() = default;
    ~RobinHoodIndex() = default;

    RobinHoodIndex(RobinHoodIndex const&) = delete;
    auto operator=(RobinHoodIndex const&) -> RobinHoodIndex& = delete;

    RobinHoodIndex(RobinHoodIndex&& other) noexcept
        : slots_(std::move(other.slots_))
        , capacity_(std::exchange(other.capacity_, 0))
        , count_(std::exchange(other.count_, 0))
    {
    }

    auto operator=(RobinHoodIndex&& other) noexcept -> RobinHoodIndex&
    {
        if (this != &other) {
            slots_ = std::move(other.slots_);
            capacity_ = std::exchange(other.capacity_, 0);
            count_ = std::exchange(other.count_, 0);
        }
        return *this;
    }

    template<typename Eq>
    [[nodiscard]]
    auto find(std::uint32_t hash, Eq&& matches) const -> std::optional<std::uint32_t>
    {
        if (capacity_ == 0) {
            return std::nullopt;
        }

        auto const h = fix_hash(hash);
        auto const mask = capacity_ - 1;
        auto slot = static_cast<std::size_t>(h) & mask;
        auto disp = std::uint16_t { 0 };

        for (;;) {
            if (meta()[slot].hash == 0) {
                return std::nullopt;
            }
            if (meta()[slot].displacement < disp) {
                return std::nullopt;
            }
            if (meta()[slot].hash == h && matches(values()[slot])) {
                return values()[slot];
            }
            ++disp;
            slot = (slot + 1) & mask;
        }
    }

    /// Insert a value under a hash. The caller guarantees the (hash, key)
    /// pair is not already present.
    auto insert(std::uint32_t hash, std::uint32_t value) -> void;

    auto reserve(std::size_t count) -> void;
    auto clear() -> void;

    [[nodiscard]]
    auto count() const -> std::size_t
    {
        return count_;
    }

private:
    struct Meta {
        std::uint32_t hash = 0;
        std::uint16_t displacement = 0;
    };

    [[nodiscard]]
    auto meta() const -> Meta*
    {
        return static_cast<Meta*>(slots_.data());
    }

    [[nodiscard]]
    auto values() const -> std::uint32_t*
    {
        return reinterpret_cast<std::uint32_t*>(meta() + capacity_);
    }

    // 0 is the empty-slot sentinel (and 1 stays reserved), so caller
    // hashes 0/1 are remapped and never collide with it.
    static auto fix_hash(std::uint32_t h) -> std::uint32_t
    {
        return h < 2 ? h + 2 : h;
    }

    auto probe_insert(std::uint32_t h, std::uint32_t value) -> void;
    auto rebuild(std::size_t new_cap) -> void;

    Region slots_;
    std::size_t capacity_ = 0;
    std::size_t count_ = 0;
};

} // namespace pup
