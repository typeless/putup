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

struct NodeIdArenaIndex {
    NodeIdMap32 offsets;
    NodeIdMap32 lengths;

    auto get_slice(NodeId id) const -> ArenaSlice
    {
        if (!offsets.contains(id))
            return { 0, 0 };
        return { offsets.get(id), lengths.get(id) };
    }

    auto set_slice(NodeId id, ArenaSlice s) -> void
    {
        offsets.set(id, s.offset);
        lengths.set(id, s.length);
    }

    auto contains(NodeId id) const -> bool { return offsets.contains(id); }

    auto resize_files(std::uint32_t n) -> void
    {
        offsets.resize_files(n);
        lengths.resize_files(n);
    }
    auto resize_commands(std::uint32_t n) -> void
    {
        offsets.resize_commands(n);
        lengths.resize_commands(n);
    }
    auto resize_conditions(std::uint32_t n) -> void
    {
        offsets.resize_conditions(n);
        lengths.resize_conditions(n);
    }
    auto resize_phis(std::uint32_t n) -> void
    {
        offsets.resize_phis(n);
        lengths.resize_phis(n);
    }

    auto clear() -> void
    {
        offsets.clear();
        lengths.clear();
    }
};

} // namespace pup
