// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors
//
// Benchmarks for worst-case algorithmic scenarios.
// Run with: ./build/test/unit/pup_test "[.benchmark]"

#include "catch_amalgamated.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/topo.hpp"
#include "pup/parser/glob.hpp"

#include <fmt/format.h>

using namespace pup::graph;
using namespace pup::parser;
using pup::NodeType;

namespace {

auto collect_node_ids(BuildGraph const& graph) -> std::vector<pup::NodeId>
{
    auto ids = std::vector<pup::NodeId> {};
    for (auto id : graph.nodes_of_type(NodeType::File))
        ids.push_back(id);
    for (auto id : graph.nodes_of_type(NodeType::Command))
        ids.push_back(id);
    for (auto id : graph.nodes_of_type(NodeType::Generated))
        ids.push_back(id);
    return ids;
}

auto generate_linear_graph(std::size_t n) -> BuildGraph
{
    auto graph = BuildGraph {};
    for (auto i = std::size_t { 0 }; i < n; ++i) {
        (void)graph.add_node(Node { .type = NodeType::File,
            .name = fmt::format("file_{}.c", i) });
    }
    return graph;
}

auto generate_order_only_graph(std::size_t n_commands)
    -> std::pair<BuildGraph, pup::NodeId>
{
    auto graph = BuildGraph {};
    auto header = graph.add_node(Node { .type = NodeType::File, .name = "common.h" });

    for (auto i = std::size_t { 0 }; i < n_commands; ++i) {
        auto cmd = graph.add_node(Node { .type = NodeType::Command,
            .command = fmt::format("gcc_{}", i) });
        (void)graph.add_order_only_edge(*header, *cmd);
    }
    return { std::move(graph), *header };
}

auto generate_wide_graph_with_order_only(std::size_t width, std::size_t depth) -> BuildGraph
{
    auto graph = BuildGraph {};

    // Create a shared order-only dependency
    auto shared_result = graph.add_node(Node { .type = NodeType::File, .name = "shared.h" });
    auto shared = *shared_result;

    // Create 'width' independent chains of 'depth' nodes
    for (auto w = std::size_t { 0 }; w < width; ++w) {
        auto prev = std::optional<pup::NodeId> {};
        for (auto d = std::size_t { 0 }; d < depth; ++d) {
            auto node_result = graph.add_node(Node { .type = NodeType::Command,
                .command = fmt::format("cmd_{}_{}", w, d) });
            auto node = *node_result;

            // Connect to previous in chain
            if (prev)
                (void)graph.add_edge(*prev, node);

            // Add order-only dep on shared header
            (void)graph.add_order_only_edge(shared, node);

            prev = node;
        }
    }
    return graph;
}

} // namespace

// =============================================================================
// Benchmark 1: get_node() lookup - Should show O(n) behavior per lookup
// With n lookups on n nodes, total is O(n²)
// =============================================================================

TEST_CASE("Benchmark: get_node lookup scaling", "[.benchmark][graph]")
{
    BENCHMARK_ADVANCED("1k nodes - all lookups")(Catch::Benchmark::Chronometer meter)
    {
        auto graph = generate_linear_graph(1000);
        auto ids = collect_node_ids(graph);
        meter.measure([&] {
            auto sum = std::size_t { 0 };
            for (auto id : ids) {
                auto const* node = graph.get_node(id);
                if (node)
                    sum += graph.get_full_path(node->id).size();
            }
            return sum;
        });
    };

    BENCHMARK_ADVANCED("10k nodes - all lookups")(Catch::Benchmark::Chronometer meter)
    {
        auto graph = generate_linear_graph(10000);
        auto ids = collect_node_ids(graph);
        meter.measure([&] {
            auto sum = std::size_t { 0 };
            for (auto id : ids) {
                auto const* node = graph.get_node(id);
                if (node)
                    sum += graph.get_full_path(node->id).size();
            }
            return sum;
        });
    };

    BENCHMARK_ADVANCED("20k nodes - all lookups")(Catch::Benchmark::Chronometer meter)
    {
        auto graph = generate_linear_graph(20000);
        auto ids = collect_node_ids(graph);
        meter.measure([&] {
            auto sum = std::size_t { 0 };
            for (auto id : ids) {
                auto const* node = graph.get_node(id);
                if (node)
                    sum += graph.get_full_path(node->id).size();
            }
            return sum;
        });
    };
}

// =============================================================================
// Benchmark 2: get_order_only_dependents() - Should show O(n) per call
// =============================================================================

TEST_CASE("Benchmark: order-only dependent lookup", "[.benchmark][graph]")
{
    BENCHMARK_ADVANCED("100 commands")(Catch::Benchmark::Chronometer meter)
    {
        auto [graph, header_id] = generate_order_only_graph(100);
        meter.measure([&] {
            return graph.get_order_only_dependents(header_id);
        });
    };

    BENCHMARK_ADVANCED("1k commands")(Catch::Benchmark::Chronometer meter)
    {
        auto [graph, header_id] = generate_order_only_graph(1000);
        meter.measure([&] {
            return graph.get_order_only_dependents(header_id);
        });
    };

    BENCHMARK_ADVANCED("10k commands")(Catch::Benchmark::Chronometer meter)
    {
        auto [graph, header_id] = generate_order_only_graph(10000);
        meter.measure([&] {
            return graph.get_order_only_dependents(header_id);
        });
    };
}

// =============================================================================
// Benchmark 3: Topological sort with order-only edges - Should show O(V² × k)
// =============================================================================

TEST_CASE("Benchmark: topo sort with order-only edges", "[.benchmark][graph]")
{
    BENCHMARK_ADVANCED("10 wide × 10 deep")(Catch::Benchmark::Chronometer meter)
    {
        auto graph = generate_wide_graph_with_order_only(10, 10);
        meter.measure([&] {
            return topological_sort(graph);
        });
    };

    BENCHMARK_ADVANCED("50 wide × 10 deep")(Catch::Benchmark::Chronometer meter)
    {
        auto graph = generate_wide_graph_with_order_only(50, 10);
        meter.measure([&] {
            return topological_sort(graph);
        });
    };

    BENCHMARK_ADVANCED("100 wide × 10 deep")(Catch::Benchmark::Chronometer meter)
    {
        auto graph = generate_wide_graph_with_order_only(100, 10);
        meter.measure([&] {
            return topological_sort(graph);
        });
    };

    BENCHMARK_ADVANCED("200 wide × 10 deep")(Catch::Benchmark::Chronometer meter)
    {
        auto graph = generate_wide_graph_with_order_only(200, 10);
        meter.measure([&] {
            return topological_sort(graph);
        });
    };
}

// =============================================================================
// Benchmark 4: Glob matching with ** backtracking - Should show exponential
// =============================================================================

TEST_CASE("Benchmark: glob ** backtracking", "[.benchmark][glob]")
{
    // Deep path that won't match the patterns (forces full backtracking)
    auto const deep_path = std::string { "a/x/x/x/x/b/y/y/y/y/c/z/z/z/z/d/w/w/w/w/file.txt" };

    BENCHMARK("single ** (no match)")
    {
        auto glob = Glob { "a/**/nomatch.txt" };
        return glob.matches(deep_path);
    };

    BENCHMARK("double ** (no match)")
    {
        auto glob = Glob { "a/**/b/**/nomatch.txt" };
        return glob.matches(deep_path);
    };

    BENCHMARK("triple ** (no match)")
    {
        auto glob = Glob { "a/**/b/**/c/**/nomatch.txt" };
        return glob.matches(deep_path);
    };

    BENCHMARK("quad ** (no match)")
    {
        auto glob = Glob { "a/**/b/**/c/**/d/**/nomatch.txt" };
        return glob.matches(deep_path);
    };
}

TEST_CASE("Benchmark: glob ** with successful match", "[.benchmark][glob]")
{
    auto const deep_path = std::string { "a/x/x/x/x/b/y/y/y/y/c/z/z/z/z/d/w/w/w/w/file.txt" };

    BENCHMARK("single ** (match)")
    {
        auto glob = Glob { "a/**/file.txt" };
        return glob.matches(deep_path);
    };

    BENCHMARK("double ** (match)")
    {
        auto glob = Glob { "a/**/b/**/file.txt" };
        return glob.matches(deep_path);
    };

    BENCHMARK("triple ** (match)")
    {
        auto glob = Glob { "a/**/b/**/c/**/file.txt" };
        return glob.matches(deep_path);
    };

    BENCHMARK("quad ** (match)")
    {
        auto glob = Glob { "a/**/b/**/c/**/d/**/file.txt" };
        return glob.matches(deep_path);
    };
}

// =============================================================================
// Benchmark 5: Glob matching many paths - O(n) with potential O(n²) via get_node
// =============================================================================

TEST_CASE("Benchmark: glob match iteration", "[.benchmark][glob]")
{
    auto const pattern = std::string { "*.c" };

    BENCHMARK_ADVANCED("match 1k paths")(Catch::Benchmark::Chronometer meter)
    {
        auto paths = std::vector<std::string> {};
        for (auto i = 0; i < 1000; ++i)
            paths.push_back(fmt::format("file_{}.c", i));

        auto glob = Glob { pattern };
        meter.measure([&] {
            auto count = 0;
            for (auto const& path : paths) {
                if (glob.matches(path))
                    ++count;
            }
            return count;
        });
    };

    BENCHMARK_ADVANCED("match 10k paths")(Catch::Benchmark::Chronometer meter)
    {
        auto paths = std::vector<std::string> {};
        for (auto i = 0; i < 10000; ++i)
            paths.push_back(fmt::format("file_{}.c", i));

        auto glob = Glob { pattern };
        meter.measure([&] {
            auto count = 0;
            for (auto const& path : paths) {
                if (glob.matches(path))
                    ++count;
            }
            return count;
        });
    };
}
