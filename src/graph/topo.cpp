// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/topo.hpp"

#include "pup/core/node_id_map.hpp"

#include <algorithm>
#include <stack>

namespace pup::graph {

namespace {

// Values match NodeIdMap32::get() default (0 = unvisited)
constexpr auto WHITE = std::uint32_t { 0 };
constexpr auto GRAY = std::uint32_t { 1 };
constexpr auto BLACK = std::uint32_t { 2 };

struct DfsState {
    NodeIdMap32 color;
    NodeIdMap32 parent;
    std::vector<NodeId> order;
    std::vector<NodeId> cycle;
    bool has_cycle = false;
};

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
        if (state.color.get(v) == WHITE) {
            state.parent.set(v, u);
            dfs_visit(graph, v, state);
        } else if (state.color.get(v) == GRAY) {
            state.has_cycle = true;
            state.cycle.clear();
            state.cycle.push_back(v);
            auto curr = u;
            while (curr != v) {
                state.cycle.push_back(curr);
                curr = state.parent.get(curr);
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
    state.color.set(u, GRAY);
    visit_neighbors(graph, u, graph.get_outputs(u), state);
    visit_neighbors(graph, u, graph.get_order_only_dependents(u), state);
    if (!state.has_cycle) {
        state.color.set(u, BLACK);
        state.order.push_back(u);
    }
}

} // namespace

auto topological_sort(BuildGraph const& graph) -> TopoSortResult
{
    auto state = DfsState {};

    for (auto id : graph.all_nodes()) {
        state.color.set(id, WHITE);
    }

    for (auto id : graph.all_nodes()) {
        if (state.color.get(id) == WHITE) {
            dfs_visit(graph, id, state);
        }
        if (state.has_cycle) {
            break;
        }
    }

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

    auto visited = NodeIdMap32 {};
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

        visited.set(u, 1);

        for (auto v : graph.get_outputs(u)) {
            if (!visited.contains(v)) {
                stack.push(v);
            }
        }
    }

    return false;
}

} // namespace pup::graph
