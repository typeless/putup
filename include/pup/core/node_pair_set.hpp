// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/robin_hood_index.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace pup {

/// Insertion-ordered set of node id pairs, for deduplicating edges as they are
/// discovered. Membership is a hash probe rather than a search over sorted
/// storage, so building a set of N pairs stays linear.
class NodeIdPairSet final {
public:
    /// Record the pair. Returns true if it was not already present.
    auto insert(NodeId from, NodeId to) -> bool
    {
        auto const h = hash_pair(from, to);
        auto const found = index_.find(h, [&](std::uint32_t slot) {
            return pairs_[slot].first == from && pairs_[slot].second == to;
        });
        if (found) {
            return false;
        }

        index_.insert(h, static_cast<std::uint32_t>(pairs_.size()));
        pairs_.push_back(std::pair { from, to });
        return true;
    }

    [[nodiscard]]
    auto size() const -> std::size_t
    {
        return pairs_.size();
    }

    auto reserve(std::size_t count) -> void
    {
        pairs_.reserve(count);
        index_.reserve(count);
    }

private:
    static auto hash_pair(NodeId from, NodeId to) -> std::uint32_t
    {
        auto x = (static_cast<std::uint64_t>(from) << 32) | to;
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return static_cast<std::uint32_t>(x);
    }

    Vec<std::pair<NodeId, NodeId>> pairs_;
    RobinHoodIndex index_;
};

} // namespace pup
