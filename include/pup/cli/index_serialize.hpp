// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/node_id_map.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/dag.hpp"
#include "pup/index/entry.hpp"

#include <optional>
#include <string_view>
#include <utility>

namespace pup::cli {

/// Path → NodeId for the index being built, ordered by interning handle.
///
/// A type of its own rather than the bare pair vector it used to alias: `pup::index::FilesByPath`
/// is the same shape over the same data, and one alias apiece said nothing about which was which.
/// Keyed by handle, so `path` must be interned in the same pool as the index's; the walks that
/// build it also query it, so it stays sorted on every insert rather than once at the end.
struct PathIdMap {
    Vec<std::pair<StringId, NodeId>> entries = {};

    /// The id recorded at `path`, or nullopt. By value: the two recursive walks insert while a
    /// previous lookup is still in hand, and a pointer into `entries` would not survive that.
    [[nodiscard]]
    auto find(StringId path) const -> std::optional<NodeId>;

    /// Record `id` at `path`, replacing any id already there.
    auto insert(StringId path, NodeId id) -> void;
};

/// The state a build may not restate. A file this build neither examined nor produced keeps what
/// the last build that looked at it recorded: stamping a fresh stat instead asserts a currency
/// nothing verified, and the next build then compares the new state against itself (#288).
struct CarriedState {
    index::FilesByPath by_path; ///< the old index, keyed by path
    Vec<StringId> examined;     ///< sorted: what change detection stat'ed
    Vec<StringId> refreshed;    ///< sorted: outputs of the commands that ran

    /// The entry to keep for `path`, or nullptr when this build may observe the file itself.
    [[nodiscard]]
    auto carry_for(StringId path) const -> index::FileEntry const*;
};

/// Serialize non-command nodes from the build graph to the index.
/// Emits one entry per live file-space node so ids stay dense (id == position + 1).
/// Paths in `deleted_stale` are marked NodeFlags::AbsenceRouted: this build removed them and routed
/// their absence, so the next build must not read the same stat failure as news.
/// `prior_generated` is the previous record's output set, sorted by handle: a path this build has
/// no authority over is classified by that record rather than by what is on disk (#369).
[[nodiscard]]
auto serialize_graph_nodes(
    graph::BuildGraph const& state,
    std::string_view source_root,
    std::string_view config_root,
    std::string_view output_root,
    Vec<StringId> const& deleted_stale = {},
    CarriedState const& carried = {},
    Vec<StringId> const& prior_generated = {}
) -> std::pair<index::Index, PathIdMap>;

/// Serialize guard-satisfied command nodes to the index with dense ids.
/// Returns the graph-id -> index-position remap; guard-unsatisfied commands
/// are absent from it.
[[nodiscard]]
auto serialize_command_nodes(
    graph::BuildGraph const& state,
    index::Index& index,
    PathIdMap const& path_to_id,
    NodeIdMap32 const& must_rerun_cmds = {}
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
