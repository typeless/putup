// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "dag.hpp"
#include "pup/core/result.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"

#include <optional>

namespace pup::graph {

/// Result of topological sort
struct TopoSortResult {
    Vec<NodeId> order;      ///< Nodes in topological order
    bool has_cycle = false; ///< True if cycle detected
    Vec<NodeId> cycle;      ///< Cycle path if detected
};

/// Perform topological sort on the graph
/// Returns nodes in dependency order (dependencies before dependents)
[[nodiscard]]
auto topological_sort(BuildGraph const& graph) -> TopoSortResult;

/// Detect cycles in the graph
/// Returns the cycle path if found, or empty vector if no cycles
[[nodiscard]]
auto detect_cycles(BuildGraph const& graph) -> Vec<NodeId>;

/// Check if graph is a DAG (no cycles)
[[nodiscard]]
auto is_dag(BuildGraph const& graph) -> bool;

/// Check if there is a path from source to target
[[nodiscard]]
auto has_path(BuildGraph const& graph, NodeId source, NodeId target) -> bool;

} // namespace pup::graph
