// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/arena.hpp"
#include "pup/core/id_array.hpp"
#include "pup/core/types.hpp"

namespace pup {

class NodeIdMap32 {
public:
    auto resize_files(std::uint32_t max_idx) -> void { files_.resize(max_idx); }
    auto resize_commands(std::uint32_t max_idx) -> void { cmds_.resize(max_idx); }
    auto resize_conditions(std::uint32_t max_idx) -> void { conds_.resize(max_idx); }
    auto resize_phis(std::uint32_t max_idx) -> void { phis_.resize(max_idx); }

    auto set(NodeId id, std::uint32_t value) -> void
    {
        auto idx = static_cast<std::uint32_t>(node_id::index(id));
        select(id).set(idx, value);
    }

    [[nodiscard]] auto get(NodeId id) const -> std::uint32_t
    {
        auto idx = static_cast<std::uint32_t>(node_id::index(id));
        return select_const(id).get(idx);
    }

    [[nodiscard]] auto contains(NodeId id) const -> bool
    {
        auto idx = static_cast<std::uint32_t>(node_id::index(id));
        return select_const(id).contains(idx);
    }

    auto clear() -> void
    {
        files_.clear();
        cmds_.clear();
        conds_.clear();
        phis_.clear();
    }

    [[nodiscard]] auto size() const -> std::size_t
    {
        return files_.count() + cmds_.count() + conds_.count() + phis_.count();
    }

    [[nodiscard]] auto empty() const -> bool { return size() == 0; }

private:
    IdArray32 files_, cmds_, conds_, phis_;

    auto select(NodeId id) -> IdArray32&
    {
        if (node_id::is_command(id)) return cmds_;
        if (node_id::is_condition(id)) return conds_;
        if (node_id::is_phi(id)) return phis_;
        return files_;
    }

    auto select_const(NodeId id) const -> IdArray32 const&
    {
        if (node_id::is_command(id)) return cmds_;
        if (node_id::is_condition(id)) return conds_;
        if (node_id::is_phi(id)) return phis_;
        return files_;
    }
};

/// Maps NodeId → ArenaSlice via two parallel NodeIdMap32 (offset + length).
/// Absent nodes return {0, 0}. Invariant: set_slice is never called with
/// length == 0, so length == 0 reliably means "not present".
class NodeIdArenaIndex {
public:
    [[nodiscard]] auto get_slice(NodeId id) const -> ArenaSlice
    {
        if (!offsets_.contains(id))
            return { 0, 0 };
        return { offsets_.get(id), lengths_.get(id) };
    }

    auto set_slice(NodeId id, ArenaSlice s) -> void
    {
        offsets_.set(id, s.offset);
        lengths_.set(id, s.length);
    }

    [[nodiscard]] auto contains(NodeId id) const -> bool { return offsets_.contains(id); }

    auto resize_files(std::uint32_t n) -> void { offsets_.resize_files(n); lengths_.resize_files(n); }
    auto resize_commands(std::uint32_t n) -> void { offsets_.resize_commands(n); lengths_.resize_commands(n); }
    auto resize_conditions(std::uint32_t n) -> void { offsets_.resize_conditions(n); lengths_.resize_conditions(n); }
    auto resize_phis(std::uint32_t n) -> void { offsets_.resize_phis(n); lengths_.resize_phis(n); }

    auto clear() -> void { offsets_.clear(); lengths_.clear(); }

private:
    NodeIdMap32 offsets_;
    NodeIdMap32 lengths_;
};

} // namespace pup
