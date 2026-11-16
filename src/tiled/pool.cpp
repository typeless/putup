// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#include "tiled/pool.hpp"

#include <utility>

namespace tiled {

namespace {

constexpr auto MAX_DEPTH = 62;
constexpr auto FULL = std::uint8_t { 254 };
constexpr auto TAKEN = std::uint8_t { 255 };
constexpr auto NONE_FREE = -1;

auto largest_free(std::uint8_t node_value, int node_log2) -> int
{
    if (node_value == FULL || node_value == TAKEN) {
        return NONE_FREE;
    }
    return node_log2 - node_value;
}

} // namespace

Pool::Pool(std::uint8_t* metadata, std::uint64_t base, std::uint8_t pool_log2, std::uint8_t min_log2)
    : metadata_(metadata)
    , base_(base)
    , pool_log2_(pool_log2)
    , min_log2_(min_log2)
{
}

Pool::Pool(Pool&& other) noexcept
    : metadata_(std::exchange(other.metadata_, nullptr))
    , base_(other.base_)
    , pool_log2_(other.pool_log2_)
    , min_log2_(other.min_log2_)
{
}

auto Pool::make(std::span<std::uint8_t> metadata, std::uint64_t base, std::uint8_t pool_log2, std::uint8_t min_log2)
    -> std::optional<Pool>
{
    if (pool_log2 >= 64 || min_log2 > pool_log2 || pool_log2 - min_log2 > MAX_DEPTH
        || metadata.size() != metadata_bytes(pool_log2, min_log2) || base % (std::uint64_t { 1 } << pool_log2) != 0) {
        return std::nullopt;
    }
    return Pool { metadata.data(), base, pool_log2, min_log2 };
}

auto Pool::acquire(std::uint8_t size_log2) -> std::optional<std::uint64_t>
{
    if (size_log2 < min_log2_ || size_log2 > pool_log2_ || largest_free(metadata_[1], pool_log2_) < size_log2) {
        return std::nullopt;
    }
    auto node = std::uint64_t { 1 };
    for (auto node_log2 = int { pool_log2_ }; node_log2 > size_log2; --node_log2) {
        auto const left = largest_free(metadata_[node * 2], node_log2 - 1);
        auto const right = largest_free(metadata_[node * 2 + 1], node_log2 - 1);
        auto const left_fits = left >= size_log2;
        auto const right_fits = right >= size_log2;
        auto const take_right = !left_fits || (right_fits && right < left);
        node = node * 2 + (take_right ? 1 : 0);
    }
    metadata_[node] = TAKEN;
    auto const offset = (node - (std::uint64_t { 1 } << (pool_log2_ - size_log2))) << size_log2;
    recompute_ancestors(node, size_log2);
    return offset;
}

auto Pool::release(std::uint64_t offset, std::uint8_t size_log2) -> bool
{
    if (size_log2 < min_log2_ || size_log2 > pool_log2_) {
        return false;
    }
    if ((offset >> pool_log2_) != 0 || (offset & ((std::uint64_t { 1 } << size_log2) - 1)) != 0) {
        return false;
    }
    auto const node = (std::uint64_t { 1 } << (pool_log2_ - size_log2)) + (offset >> size_log2);
    if (metadata_[node] != TAKEN) {
        return false;
    }
    metadata_[node] = 0;
    recompute_ancestors(node, size_log2);
    return true;
}

auto Pool::recompute_ancestors(std::uint64_t node, int node_log2) -> void
{
    while (node > 1) {
        node /= 2;
        ++node_log2;
        auto const left_value = metadata_[node * 2];
        auto const right_value = metadata_[node * 2 + 1];
        if (left_value == 0 && right_value == 0) {
            metadata_[node] = 0;
            continue;
        }
        auto const left = largest_free(left_value, node_log2 - 1);
        auto const right = largest_free(right_value, node_log2 - 1);
        auto const largest = left > right ? left : right;
        metadata_[node] = largest == NONE_FREE ? FULL : static_cast<std::uint8_t>(node_log2 - largest);
    }
}

} // namespace tiled
