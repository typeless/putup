// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

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

class NodeIdMap64 {
public:
    auto resize_files(std::uint32_t max_idx) -> void { files_.resize(max_idx); }
    auto resize_commands(std::uint32_t max_idx) -> void { cmds_.resize(max_idx); }
    auto resize_conditions(std::uint32_t max_idx) -> void { conds_.resize(max_idx); }
    auto resize_phis(std::uint32_t max_idx) -> void { phis_.resize(max_idx); }

    auto set(NodeId id, std::uint64_t value) -> void
    {
        auto idx = static_cast<std::uint32_t>(node_id::index(id));
        select(id).set(idx, value);
    }

    [[nodiscard]] auto get(NodeId id) const -> std::uint64_t
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
    IdArray64 files_, cmds_, conds_, phis_;

    auto select(NodeId id) -> IdArray64&
    {
        if (node_id::is_command(id)) return cmds_;
        if (node_id::is_condition(id)) return conds_;
        if (node_id::is_phi(id)) return phis_;
        return files_;
    }

    auto select_const(NodeId id) const -> IdArray64 const&
    {
        if (node_id::is_command(id)) return cmds_;
        if (node_id::is_condition(id)) return conds_;
        if (node_id::is_phi(id)) return phis_;
        return files_;
    }
};

} // namespace pup
