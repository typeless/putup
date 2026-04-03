// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/topo.hpp"

#include "pup/core/node_id_map.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/dag.hpp"

#include <algorithm>
#include <cstdint>
#include <stack>
#include <utility>

namespace pup::graph {

namespace {

// Values match NodeIdMap32::get() default (0 = unvisited)
constexpr auto WHITE = std::uint32_t { 0 };
constexpr auto GRAY = std::uint32_t { 1 };
constexpr auto BLACK = std::uint32_t { 2 };

struct DfsState {
    NodeIdMap32 color;
    NodeIdMap32 parent;
    Vec<NodeId> order;
    Vec<NodeId> cycle;
    bool has_cycle = false;
};

auto dfs_visit(Graph const& graph, NodeId u, DfsState& state) -> void;

auto visit_neighbors(
    Graph const& graph,
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

auto dfs_visit(Graph const& graph, NodeId u, DfsState& state) -> void
{
    if (state.has_cycle) {
        return;
    }
    state.color.set(u, GRAY);
    visit_neighbors(graph, u, get_outputs(graph, u), state);
    visit_neighbors(graph, u, get_order_only_dependents(graph, u), state);
    if (!state.has_cycle) {
        state.color.set(u, BLACK);
        state.order.push_back(u);
    }
}

} // namespace

auto topological_sort(Graph const& graph) -> TopoSortResult
{
    auto state = DfsState {};

    for (auto id : all_nodes(graph)) {
        state.color.set(id, WHITE);
    }

    for (auto id : all_nodes(graph)) {
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

auto detect_cycles(Graph const& graph) -> Vec<NodeId>
{
    auto result = TopoSortResult { topological_sort(graph) };
    return result.cycle;
}

auto is_dag(Graph const& graph) -> bool
{
    return detect_cycles(graph).empty();
}

auto has_path(Graph const& graph, NodeId source, NodeId target) -> bool
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

        for (auto v : get_outputs(graph, u)) {
            if (!visited.contains(v)) {
                stack.push(v);
            }
        }
    }

    return false;
}

} // namespace pup::graph
