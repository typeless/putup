// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/topo.hpp"

#include <algorithm>
#include <stack>
#include <unordered_map>
#include <unordered_set>

namespace pup::graph {

namespace {

enum class Color { White,
                   Gray,
                   Black };

struct DfsState {
    std::unordered_map<NodeId, Color> color;
    std::unordered_map<NodeId, NodeId> parent;
    std::vector<NodeId> order;
    std::vector<NodeId> cycle;
    bool has_cycle = false;
};

// Forward declaration for mutual recursion
auto dfs_visit(BuildGraph const& graph, NodeId u, DfsState& state) -> void;

auto visit_neighbors(
    BuildGraph const& graph,
    NodeId u,
    auto const& neighbors,
    DfsState& state
) -> void
{
    for (auto v : neighbors) {
        if (state.has_cycle) {
            return;
        }
        if (state.color[v] == Color::White) {
            state.parent[v] = u;
            dfs_visit(graph, v, state);
        } else if (state.color[v] == Color::Gray) {
            state.has_cycle = true;
            state.cycle.clear();
            state.cycle.push_back(v);
            auto curr = u;
            while (curr != v) {
                state.cycle.push_back(curr);
                curr = state.parent[curr];
            }
            state.cycle.push_back(v);
            std::ranges::reverse(state.cycle);
        }
    }
}

auto dfs_visit(BuildGraph const& graph, NodeId u, DfsState& state) -> void
{
    if (state.has_cycle) {
        return;
    }
    state.color[u] = Color::Gray;
    visit_neighbors(graph, u, graph.get_outputs(u), state);
    visit_neighbors(graph, u, graph.get_order_only_dependents(u), state);
    if (!state.has_cycle) {
        state.color[u] = Color::Black;
        state.order.push_back(u);
    }
}

} // namespace

auto topological_sort(BuildGraph const& graph) -> TopoSortResult
{
    auto state = DfsState {};

    // Initialize all nodes as white (unvisited)
    for (auto id : graph.all_nodes()) {
        state.color[id] = Color::White;
    }

    // DFS from all unvisited nodes
    for (auto id : graph.all_nodes()) {
        if (state.color[id] == Color::White) {
            dfs_visit(graph, id, state);
        }
        if (state.has_cycle) {
            break;
        }
    }

    // Reverse for topological order (dependencies first)
    std::ranges::reverse(state.order);

    return TopoSortResult {
        .order = std::move(state.order),
        .has_cycle = state.has_cycle,
        .cycle = std::move(state.cycle),
    };
}

auto detect_cycles(BuildGraph const& graph) -> std::vector<NodeId>
{
    auto result = TopoSortResult { topological_sort(graph) };
    return result.cycle;
}

auto is_dag(BuildGraph const& graph) -> bool
{
    return detect_cycles(graph).empty();
}

auto has_path(BuildGraph const& graph, NodeId source, NodeId target) -> bool
{
    if (source == target) {
        return true;
    }

    auto visited = std::unordered_set<NodeId> {};
    auto stack = std::stack<NodeId> {};

    stack.push(source);

    while (!stack.empty()) {
        auto u = NodeId { stack.top() };
        stack.pop();

        if (u == target) {
            return true;
        }

        if (visited.contains(u)) {
            continue;
        }

        visited.insert(u);

        for (auto v : graph.get_outputs(u)) {
            if (!visited.contains(v)) {
                stack.push(v);
            }
        }
    }

    return false;
}

} // namespace pup::graph
