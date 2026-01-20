// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/topo.hpp"

#include <algorithm>
#include <queue>
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

auto visit_neighbors(BuildGraph const& graph, NodeId u, auto const& neighbors, DfsState& state) -> void
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

auto reverse_topological_sort(BuildGraph const& graph) -> TopoSortResult
{
    auto result = TopoSortResult { topological_sort(graph) };
    std::ranges::reverse(result.order);
    return result;
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

auto reachable_from(BuildGraph const& graph, NodeId start) -> std::vector<NodeId>
{
    auto visited = std::unordered_set<NodeId> {};
    auto result = std::vector<NodeId> {};
    auto stack = std::stack<NodeId> {};

    stack.push(start);

    while (!stack.empty()) {
        auto u = NodeId { stack.top() };
        stack.pop();

        if (visited.contains(u)) {
            continue;
        }

        visited.insert(u);
        result.push_back(u);

        for (auto v : graph.get_outputs(u)) {
            if (!visited.contains(v)) {
                stack.push(v);
            }
        }
    }

    return result;
}

auto can_reach(BuildGraph const& graph, NodeId target) -> std::vector<NodeId>
{
    auto visited = std::unordered_set<NodeId> {};
    auto result = std::vector<NodeId> {};
    auto stack = std::stack<NodeId> {};

    stack.push(target);

    while (!stack.empty()) {
        auto u = NodeId { stack.top() };
        stack.pop();

        if (visited.contains(u)) {
            continue;
        }

        visited.insert(u);
        result.push_back(u);

        for (auto v : graph.get_inputs(u)) {
            if (!visited.contains(v)) {
                stack.push(v);
            }
        }
    }

    return result;
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

auto nodes_at_depth(BuildGraph const& graph, std::size_t depth) -> std::vector<NodeId>
{
    auto result = std::vector<NodeId> {};

    // BFS from roots
    auto visited = std::unordered_set<NodeId> {};
    auto queue = std::queue<std::pair<NodeId, std::size_t>> {};

    // Start from root nodes
    for (auto id : graph.root_nodes()) {
        queue.emplace(id, 0);
        visited.insert(id);
    }

    while (!queue.empty()) {
        auto [u, d] = queue.front();
        queue.pop();

        if (d == depth) {
            result.push_back(u);
        }

        if (d < depth) {
            for (auto v : graph.get_outputs(u)) {
                if (!visited.contains(v)) {
                    visited.insert(v);
                    queue.emplace(v, d + 1);
                }
            }
        }
    }

    return result;
}

auto node_depth(BuildGraph const& graph, NodeId id) -> std::size_t
{
    // Find longest path from any root to this node
    auto depths = std::unordered_map<NodeId, std::size_t> {};

    // Topological sort first
    auto sorted = TopoSortResult { topological_sort(graph) };
    if (sorted.has_cycle) {
        return 0;
    }

    // Initialize root depths
    for (auto root : graph.root_nodes()) {
        depths[root] = 0;
    }

    // Process in topological order
    for (auto u : sorted.order) {
        if (!depths.contains(u)) {
            depths[u] = 0;
        }

        for (auto v : graph.get_outputs(u)) {
            depths[v] = std::max(depths[v], depths[u] + 1);
        }
    }

    return depths.contains(id) ? depths[id] : 0;
}

auto max_depth(BuildGraph const& graph) -> std::size_t
{
    auto sorted = TopoSortResult { topological_sort(graph) };
    if (sorted.has_cycle) {
        return 0;
    }

    auto depths = std::unordered_map<NodeId, std::size_t> {};

    for (auto root : graph.root_nodes()) {
        depths[root] = 0;
    }

    auto max_d = std::size_t { 0 };

    for (auto u : sorted.order) {
        if (!depths.contains(u)) {
            depths[u] = 0;
        }

        for (auto v : graph.get_outputs(u)) {
            depths[v] = std::max(depths[v], depths[u] + 1);
            max_d = std::max(max_d, depths[v]);
        }
    }

    return max_d;
}

auto critical_path(BuildGraph const& graph) -> std::vector<NodeId>
{
    auto sorted = TopoSortResult { topological_sort(graph) };
    if (sorted.has_cycle) {
        return {};
    }

    // Forward pass: compute longest path to each node
    auto dist = std::unordered_map<NodeId, std::size_t> {};
    auto pred = std::unordered_map<NodeId, NodeId> {};

    for (auto root : graph.root_nodes()) {
        dist[root] = 0;
    }

    for (auto u : sorted.order) {
        if (!dist.contains(u)) {
            dist[u] = 0;
        }

        for (auto v : graph.get_outputs(u)) {
            if (!dist.contains(v) || dist[u] + 1 > dist[v]) {
                dist[v] = dist[u] + 1;
                pred[v] = u;
            }
        }
    }

    // Find the node with maximum distance (end of critical path)
    auto max_dist = std::size_t { 0 };
    auto end_node = NodeId { 0 };
    for (auto const& [id, d] : dist) {
        if (d >= max_dist) {
            max_dist = d;
            end_node = id;
        }
    }

    if (end_node == 0) {
        return {};
    }

    // Backtrack to find the path
    auto path = std::vector<NodeId> {};
    auto curr = end_node;
    while (curr != 0) {
        path.push_back(curr);
        if (!pred.contains(curr)) {
            break;
        }
        curr = pred[curr];
    }

    std::ranges::reverse(path);
    return path;
}

} // namespace pup::graph
