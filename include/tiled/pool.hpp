// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace tiled {

class Pool final {
public:
    [[nodiscard]]
    static auto make(std::span<std::uint8_t> metadata, std::uint8_t pool_log2, std::uint8_t min_log2)
        -> std::optional<Pool>;

    [[nodiscard]]
    static constexpr auto metadata_bytes(std::uint8_t pool_log2, std::uint8_t min_log2) -> std::uint64_t
    {
        return std::uint64_t { 2 } << (pool_log2 - min_log2);
    }

    [[nodiscard]]
    auto acquire(std::uint8_t size_log2) -> std::optional<std::uint64_t>;

    [[nodiscard]]
    auto release(std::uint64_t offset, std::uint8_t size_log2) -> bool;

    Pool(Pool const&) = delete;
    auto operator=(Pool const&) -> Pool& = delete;
    Pool(Pool&& other) noexcept;
    auto operator=(Pool&&) -> Pool& = delete;
    ~Pool() = default;

private:
    Pool(std::uint8_t* metadata, std::uint8_t pool_log2, std::uint8_t min_log2);

    auto recompute_ancestors(std::uint64_t node, int node_log2) -> void;

    std::uint8_t* metadata_;
    std::uint8_t pool_log2_;
    std::uint8_t min_log2_;
};

} // namespace tiled
