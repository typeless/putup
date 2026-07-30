// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/node_id_map.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/dag.hpp"
#include "pup/index/entry.hpp"

#include <string_view>
#include <utility>

namespace pup::cli {

/// Path string → NodeId mapping built during serialization
using PathIdMap = Vec<std::pair<StringId, NodeId>>;

/// Serialize non-command nodes from the build graph to the index.
/// Emits one entry per live file-space node so ids stay dense (id == position + 1).
[[nodiscard]]
auto serialize_graph_nodes(
    graph::BuildGraph const& state,
    std::string_view source_root,
    std::string_view config_root,
    std::string_view output_root
) -> std::pair<index::Index, PathIdMap>;

/// Serialize guard-satisfied command nodes to the index with dense ids.
/// Returns the graph-id -> index-position remap; guard-unsatisfied commands
/// are absent from it.
[[nodiscard]]
auto serialize_command_nodes(
    graph::BuildGraph const& state,
    index::Index& index,
    PathIdMap const& path_to_id,
    NodeIdMap32 const& failed_cmds = {}
) -> NodeIdMap32;

/// Serialize graph edges to the index, rewriting command endpoints through the
/// remap and dropping edges whose command endpoint is absent from it.
/// Every edge type is persisted, order-only included — removal routing reads them back (#166).
auto serialize_edges(
    graph::BuildGraph const& state,
    index::Index& index,
    NodeIdMap32 const& cmd_remap
) -> void;

} // namespace pup::cli
