// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "dag.hpp"
#include "pup/core/result.hpp"
#include "pup/core/types.hpp"

#include <optional>
#include <vector>

namespace pup::graph {

/// Result of topological sort
struct TopoSortResult {
    std::vector<NodeId> order; ///< Nodes in topological order
    bool has_cycle = false;    ///< True if cycle detected
    std::vector<NodeId> cycle; ///< Cycle path if detected
};

/// Perform topological sort on the graph
/// Returns nodes in dependency order (dependencies before dependents)
[[nodiscard]]
auto topological_sort(BuildGraph const& graph) -> TopoSortResult;

/// Perform reverse topological sort (dependents before dependencies)
[[nodiscard]]
auto reverse_topological_sort(BuildGraph const& graph) -> TopoSortResult;

/// Detect cycles in the graph
/// Returns the cycle path if found, or empty vector if no cycles
[[nodiscard]]
auto detect_cycles(BuildGraph const& graph) -> std::vector<NodeId>;

/// Check if graph is a DAG (no cycles)
[[nodiscard]]
auto is_dag(BuildGraph const& graph) -> bool;

/// Get all nodes reachable from a given node (transitive closure)
[[nodiscard]]
auto reachable_from(BuildGraph const& graph, NodeId start) -> std::vector<NodeId>;

/// Get all nodes that can reach a given node (reverse transitive closure)
[[nodiscard]]
auto can_reach(BuildGraph const& graph, NodeId target) -> std::vector<NodeId>;

/// Check if there is a path from source to target
[[nodiscard]]
auto has_path(BuildGraph const& graph, NodeId source, NodeId target) -> bool;

/// Get nodes at a given depth from roots
[[nodiscard]]
auto nodes_at_depth(BuildGraph const& graph, std::size_t depth) -> std::vector<NodeId>;

/// Get the depth of a node (longest path from any root)
[[nodiscard]]
auto node_depth(BuildGraph const& graph, NodeId id) -> std::size_t;

/// Get the maximum depth of the graph
[[nodiscard]]
auto max_depth(BuildGraph const& graph) -> std::size_t;

/// Compute the critical path (longest path through the graph)
[[nodiscard]]
auto critical_path(BuildGraph const& graph) -> std::vector<NodeId>;

} // namespace pup::graph
