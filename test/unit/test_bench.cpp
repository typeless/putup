// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors
//
// Benchmarks for worst-case algorithmic scenarios.
// Run with: ./build/test/unit/pup_test "[.benchmark]"

#include "catch_amalgamated.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/topo.hpp"
#include "pup/parser/glob.hpp"

#include <cstdio>

using namespace pup::graph;
using namespace pup::parser;
using pup::NodeType;

namespace {

auto collect_node_ids(Graph const& graph) -> std::vector<pup::NodeId>
{
    auto ids = std::vector<pup::NodeId> {};
    for (auto id : nodes_of_type(graph, NodeType::File))
        ids.push_back(id);
    for (auto id : nodes_of_type(graph, NodeType::Command))
        ids.push_back(id);
    for (auto id : nodes_of_type(graph, NodeType::Generated))
        ids.push_back(id);
    return ids;
}

auto generate_linear_graph(std::size_t n) -> BuildGraph
{
    auto bs = make_build_graph();
    auto& graph = bs.graph;
    for (auto i = std::size_t { 0 }; i < n; ++i) {
        char buf[64];
        snprintf(buf, sizeof(buf), "file_%zu.c", i);
        (void)add_file_node(graph, FileNode { .type = NodeType::File,
            .name = pup::global_pool().intern(buf) });
    }
    return bs;
}

auto generate_order_only_graph(
    std::size_t n_commands
) -> std::pair<BuildGraph, pup::NodeId>
{
    auto bs = make_build_graph();
    auto& graph = bs.graph;
    auto header = add_file_node(graph, FileNode { .type = NodeType::File, .name = pup::global_pool().intern("common.h") });

    for (auto i = std::size_t { 0 }; i < n_commands; ++i) {
        char buf[64];
        snprintf(buf, sizeof(buf), "gcc_%zu", i);
        auto cmd = add_command_node(graph, CommandNode {
            .instruction_id = pup::global_pool().intern(buf) });
        (void)add_order_only_edge(graph, *header, *cmd);
    }
    return { std::move(bs), *header };
}

auto generate_wide_graph_with_order_only(std::size_t width, std::size_t depth) -> BuildGraph
{
    auto bs = make_build_graph();
    auto& graph = bs.graph;

    // Create a shared order-only dependency
    auto shared_result = add_file_node(graph, FileNode { .name = pup::global_pool().intern("shared.h") });
    auto shared = *shared_result;

    // Create 'width' independent chains of 'depth' nodes
    for (auto w = std::size_t { 0 }; w < width; ++w) {
        auto prev = std::optional<pup::NodeId> {};
        for (auto d = std::size_t { 0 }; d < depth; ++d) {
            char buf[64];
            snprintf(buf, sizeof(buf), "cmd_%zu_%zu", w, d);
            auto node_result = add_command_node(graph, CommandNode {
                .instruction_id = pup::global_pool().intern(buf) });
            auto node = *node_result;

            // Connect to previous in chain
            if (prev)
                (void)add_edge(graph, *prev, node);

            // Add order-only dep on shared header
            (void)add_order_only_edge(graph, shared, node);

            prev = node;
        }
    }
    return bs;
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
        auto bs = generate_linear_graph(1000);
        auto ids = collect_node_ids(bs.graph);
        meter.measure([&] {
            auto sum = std::size_t { 0 };
            for (auto id : ids) {
                auto const* node = get_file_node(bs.graph, id);
                if (node)
                    sum += pup::global_pool().get(get_full_path(bs.graph, node->id)).size();
            }
            return sum;
        });
    };

    BENCHMARK_ADVANCED("10k nodes - all lookups")(Catch::Benchmark::Chronometer meter)
    {
        auto bs = generate_linear_graph(10000);
        auto ids = collect_node_ids(bs.graph);
        meter.measure([&] {
            auto sum = std::size_t { 0 };
            for (auto id : ids) {
                auto const* node = get_file_node(bs.graph, id);
                if (node)
                    sum += pup::global_pool().get(get_full_path(bs.graph, node->id)).size();
            }
            return sum;
        });
    };

    BENCHMARK_ADVANCED("20k nodes - all lookups")(Catch::Benchmark::Chronometer meter)
    {
        auto bs = generate_linear_graph(20000);
        auto ids = collect_node_ids(bs.graph);
        meter.measure([&] {
            auto sum = std::size_t { 0 };
            for (auto id : ids) {
                auto const* node = get_file_node(bs.graph, id);
                if (node)
                    sum += pup::global_pool().get(get_full_path(bs.graph, node->id)).size();
            }
            return sum;
        });
    };
}

// =============================================================================
// Benchmark 2: get_order_only_dependents() - Scans all edges from a node and filters — O(fan-out)
// =============================================================================

TEST_CASE("Benchmark: order-only dependent lookup", "[.benchmark][graph]")
{
    BENCHMARK_ADVANCED("100 commands")(Catch::Benchmark::Chronometer meter)
    {
        auto [bs, header_id] = generate_order_only_graph(100);
        meter.measure([&] {
            return get_order_only_dependents(bs.graph, header_id);
        });
    };

    BENCHMARK_ADVANCED("1k commands")(Catch::Benchmark::Chronometer meter)
    {
        auto [bs, header_id] = generate_order_only_graph(1000);
        meter.measure([&] {
            return get_order_only_dependents(bs.graph, header_id);
        });
    };

    BENCHMARK_ADVANCED("10k commands")(Catch::Benchmark::Chronometer meter)
    {
        auto [bs, header_id] = generate_order_only_graph(10000);
        meter.measure([&] {
            return get_order_only_dependents(bs.graph, header_id);
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
        auto bs = generate_wide_graph_with_order_only(10, 10);
        meter.measure([&] {
            return topological_sort(bs.graph);
        });
    };

    BENCHMARK_ADVANCED("50 wide × 10 deep")(Catch::Benchmark::Chronometer meter)
    {
        auto bs = generate_wide_graph_with_order_only(50, 10);
        meter.measure([&] {
            return topological_sort(bs.graph);
        });
    };

    BENCHMARK_ADVANCED("100 wide × 10 deep")(Catch::Benchmark::Chronometer meter)
    {
        auto bs = generate_wide_graph_with_order_only(100, 10);
        meter.measure([&] {
            return topological_sort(bs.graph);
        });
    };

    BENCHMARK_ADVANCED("200 wide × 10 deep")(Catch::Benchmark::Chronometer meter)
    {
        auto bs = generate_wide_graph_with_order_only(200, 10);
        meter.measure([&] {
            return topological_sort(bs.graph);
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
        for (auto i = 0; i < 1000; ++i) {
            char buf[64];
            snprintf(buf, sizeof(buf), "file_%d.c", i);
            paths.push_back(std::string { buf });
        }

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
        for (auto i = 0; i < 10000; ++i) {
            char buf[64];
            snprintf(buf, sizeof(buf), "file_%d.c", i);
            paths.push_back(std::string { buf });
        }

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
