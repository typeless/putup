// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/topo.hpp"

#include <algorithm>

using namespace pup::graph;
using pup::NodeType;

TEST_CASE("BuildGraph basic operations", "[graph]")
{
    auto graph = BuildGraph {};

    SECTION("add nodes")
    {
        auto node1 = FileNode { .name = graph.intern("foo.c") };
        auto node2 = FileNode { .name = graph.intern("bar.c") };

        auto id1 = graph.add_file_node(node1);
        auto id2 = graph.add_file_node(node2);

        REQUIRE(id1.has_value());
        REQUIRE(id2.has_value());
        REQUIRE(*id1 != *id2);

        // Graph starts with BUILD_ROOT_ID (id=1), so after adding 2 nodes, count is 3
        REQUIRE(graph.node_count() == 3);
    }

    SECTION("find by dir and name")
    {
        // Create directory node first
        auto dir_node = FileNode { .type = NodeType::Directory, .name = graph.intern("src") };
        auto dir_id = graph.add_file_node(dir_node);
        REQUIRE(dir_id.has_value());

        // Create file node with parent_dir and name
        auto file_node = FileNode {
            .name = graph.intern("foo.c"),
            .parent_dir = *dir_id,
        };
        auto file_id = graph.add_file_node(file_node);
        REQUIRE(file_id.has_value());

        // Find by (parent_dir, name)
        auto found = graph.find_by_dir_name(*dir_id, "foo.c");
        REQUIRE(found.has_value());
        REQUIRE(*found == *file_id);

        // Not found cases
        REQUIRE_FALSE(graph.find_by_dir_name(*dir_id, "bar.c").has_value());
        REQUIRE_FALSE(graph.find_by_dir_name(0, "foo.c").has_value());
    }

    SECTION("find by dir and name - multiple directories")
    {
        // src/ and lib/ directories
        auto src_dir = graph.add_file_node(FileNode { .type = NodeType::Directory, .name = graph.intern("src") });
        auto lib_dir = graph.add_file_node(FileNode { .type = NodeType::Directory, .name = graph.intern("lib") });
        REQUIRE(src_dir.has_value());
        REQUIRE(lib_dir.has_value());

        // Same basename in different directories
        auto src_file = graph.add_file_node(FileNode {
            .name = graph.intern("util.c"),
            .parent_dir = *src_dir,
        });
        auto lib_file = graph.add_file_node(FileNode {
            .name = graph.intern("util.c"),
            .parent_dir = *lib_dir,
        });
        REQUIRE(src_file.has_value());
        REQUIRE(lib_file.has_value());

        // Each lookup returns the correct node
        auto found_src = graph.find_by_dir_name(*src_dir, "util.c");
        auto found_lib = graph.find_by_dir_name(*lib_dir, "util.c");
        REQUIRE(found_src.has_value());
        REQUIRE(found_lib.has_value());
        REQUIRE(*found_src == *src_file);
        REQUIRE(*found_lib == *lib_file);
        REQUIRE(*found_src != *found_lib);
    }

    SECTION("get_full_path - simple hierarchy")
    {
        // Build: src/foo.c
        auto src_dir = graph.add_file_node(FileNode { .type = NodeType::Directory, .name = graph.intern("src") });
        auto foo_file = graph.add_file_node(FileNode {
            .name = graph.intern("foo.c"),
            .parent_dir = *src_dir,
        });
        REQUIRE(src_dir.has_value());
        REQUIRE(foo_file.has_value());

        REQUIRE(graph.get_full_path(*src_dir) == "src");
        REQUIRE(graph.get_full_path(*foo_file) == "src/foo.c");
    }

    SECTION("get_full_path - deep hierarchy")
    {
        // Build: a/b/c/d/file.txt
        auto a_dir = graph.add_file_node(FileNode { .type = NodeType::Directory, .name = graph.intern("a") });
        auto b_dir = graph.add_file_node(FileNode { .type = NodeType::Directory, .name = graph.intern("b"), .parent_dir = *a_dir });
        auto c_dir = graph.add_file_node(FileNode { .type = NodeType::Directory, .name = graph.intern("c"), .parent_dir = *b_dir });
        auto d_dir = graph.add_file_node(FileNode { .type = NodeType::Directory, .name = graph.intern("d"), .parent_dir = *c_dir });
        auto file = graph.add_file_node(FileNode { .name = graph.intern("file.txt"), .parent_dir = *d_dir });

        REQUIRE(graph.get_full_path(*a_dir) == "a");
        REQUIRE(graph.get_full_path(*b_dir) == "a/b");
        REQUIRE(graph.get_full_path(*c_dir) == "a/b/c");
        REQUIRE(graph.get_full_path(*d_dir) == "a/b/c/d");
        REQUIRE(graph.get_full_path(*file) == "a/b/c/d/file.txt");
    }

    SECTION("get_full_path - name with path separator (no parent_dir)")
    {
        auto node = graph.add_file_node(FileNode { .name = graph.intern("legacy/path.c") });
        REQUIRE(graph.get_full_path(*node) == "legacy/path.c");
    }

    SECTION("get_full_path - root level file")
    {
        auto file = graph.add_file_node(FileNode { .name = graph.intern("Makefile") });
        REQUIRE(graph.get_full_path(*file) == "Makefile");
    }

    SECTION("get_full_path - invalid node returns empty")
    {
        REQUIRE(graph.get_full_path(0) == "");
        REQUIRE(graph.get_full_path(9999) == "");
    }

    SECTION("get_full_path - caching works")
    {
        auto dir = graph.add_file_node(FileNode { .type = NodeType::Directory, .name = graph.intern("cached") });
        auto file = graph.add_file_node(FileNode { .name = graph.intern("test.c"), .parent_dir = *dir });

        // First call computes and caches
        auto path1 = graph.get_full_path(*file);
        // Second call should hit cache
        auto path2 = graph.get_full_path(*file);

        REQUIRE(path1 == "cached/test.c");
        REQUIRE(path2 == "cached/test.c");
    }

    SECTION("invalidate_path_cache and clear_path_cache")
    {
        auto dir = graph.add_file_node(FileNode { .type = NodeType::Directory, .name = graph.intern("dir") });
        auto file = graph.add_file_node(FileNode { .name = graph.intern("file.c"), .parent_dir = *dir });

        // Populate cache
        (void)graph.get_full_path(*file);
        (void)graph.get_full_path(*dir);

        // Invalidate single entry
        graph.invalidate_path_cache(*file);
        // Should still work (recomputes)
        REQUIRE(graph.get_full_path(*file) == "dir/file.c");

        // Clear entire cache
        graph.clear_path_cache();
        // Should still work (recomputes)
        REQUIRE(graph.get_full_path(*dir) == "dir");
        REQUIRE(graph.get_full_path(*file) == "dir/file.c");
    }

    SECTION("add edges")
    {
        auto id1 = graph.add_file_node(FileNode { .name = graph.intern("foo.c") });
        auto id2 = graph.add_command_node(CommandNode { .command = graph.intern("gcc foo.c") });
        auto id3 = graph.add_file_node(FileNode { .type = NodeType::Generated, .name = graph.intern("foo.o") });

        REQUIRE(id1.has_value());
        REQUIRE(id2.has_value());
        REQUIRE(id3.has_value());

        auto e1 = graph.add_edge(*id1, *id2);
        auto e2 = graph.add_edge(*id2, *id3);

        REQUIRE(e1.has_value());
        REQUIRE(e2.has_value());

        REQUIRE(graph.edge_count() == 2);

        auto inputs = graph.get_inputs(*id2);
        REQUIRE(inputs.size() == 1);
        REQUIRE(inputs[0] == *id1);

        auto outputs = graph.get_outputs(*id2);
        REQUIRE(outputs.size() == 1);
        REQUIRE(outputs[0] == *id3);
    }

    SECTION("nodes of type")
    {
        (void)graph.add_file_node(FileNode { .name = graph.intern("a.c") });
        (void)graph.add_file_node(FileNode { .name = graph.intern("b.c") });
        (void)graph.add_command_node(CommandNode { .command = graph.intern("gcc") });
        (void)graph.add_file_node(FileNode { .type = NodeType::Generated, .name = graph.intern("out.o") });

        auto files = graph.nodes_of_type(NodeType::File);
        REQUIRE(files.size() == 2);

        auto commands = graph.nodes_of_type(NodeType::Command);
        REQUIRE(commands.size() == 1);

        auto generated = graph.nodes_of_type(NodeType::Generated);
        REQUIRE(generated.size() == 1);
    }

    SECTION("root and leaf nodes")
    {
        auto id1 = graph.add_file_node(FileNode { .name = graph.intern("a.c") });
        auto id2 = graph.add_command_node(CommandNode { .command = graph.intern("") });
        auto id3 = graph.add_file_node(FileNode { .type = NodeType::Generated, .name = graph.intern("a.o") });

        (void)graph.add_edge(*id1, *id2);
        (void)graph.add_edge(*id2, *id3);

        auto roots = graph.root_nodes();
        // Root nodes include BUILD_ROOT_ID (which has no inputs) plus a.c
        REQUIRE(roots.size() == 2);
        // One of the roots should be the file we added
        REQUIRE(std::ranges::find(roots, *id1) != roots.end());

        auto leaves = graph.leaf_nodes();
        // BUILD_ROOT_ID is also a leaf (no outputs unless used)
        REQUIRE(leaves.size() == 2);
        REQUIRE(std::ranges::find(leaves, *id3) != leaves.end());
    }
}

TEST_CASE("Topological sort", "[graph]")
{
    auto graph = BuildGraph {};

    SECTION("simple linear graph")
    {
        auto id1 = graph.add_file_node(FileNode { .name = graph.intern("a") });
        auto id2 = graph.add_file_node(FileNode { .name = graph.intern("b") });
        auto id3 = graph.add_file_node(FileNode { .name = graph.intern("c") });

        (void)graph.add_edge(*id1, *id2);
        (void)graph.add_edge(*id2, *id3);

        auto result = topological_sort(graph);
        REQUIRE_FALSE(result.has_cycle);
        // 3 nodes we added + BUILD_ROOT_ID = 4
        REQUIRE(result.order.size() == 4);

        // a should come before b, b before c
        auto pos_a = std::ranges::find(result.order, *id1);
        auto pos_b = std::ranges::find(result.order, *id2);
        auto pos_c = std::ranges::find(result.order, *id3);

        REQUIRE(pos_a < pos_b);
        REQUIRE(pos_b < pos_c);
    }

    SECTION("diamond graph")
    {
        //     a
        //    / |
        //   b   c
        //    | /
        //     d
        auto id_a = graph.add_file_node(FileNode { .name = graph.intern("a") });
        auto id_b = graph.add_file_node(FileNode { .name = graph.intern("b") });
        auto id_c = graph.add_file_node(FileNode { .name = graph.intern("c") });
        auto id_d = graph.add_file_node(FileNode { .name = graph.intern("d") });

        (void)graph.add_edge(*id_a, *id_b);
        (void)graph.add_edge(*id_a, *id_c);
        (void)graph.add_edge(*id_b, *id_d);
        (void)graph.add_edge(*id_c, *id_d);

        auto result = topological_sort(graph);
        REQUIRE_FALSE(result.has_cycle);
        // 4 nodes we added + BUILD_ROOT_ID = 5
        REQUIRE(result.order.size() == 5);

        // a should come before d, d should come last among our nodes
        auto pos_a = std::ranges::find(result.order, *id_a);
        auto pos_d = std::ranges::find(result.order, *id_d);
        REQUIRE(pos_a < pos_d);
    }

    SECTION("cycle detection")
    {
        auto id1 = graph.add_file_node(FileNode { .name = graph.intern("a") });
        auto id2 = graph.add_file_node(FileNode { .name = graph.intern("b") });
        auto id3 = graph.add_file_node(FileNode { .name = graph.intern("c") });

        (void)graph.add_edge(*id1, *id2);
        (void)graph.add_edge(*id2, *id3);
        (void)graph.add_edge(*id3, *id1); // Creates cycle

        auto result = topological_sort(graph);
        REQUIRE(result.has_cycle);
        REQUIRE_FALSE(result.cycle.empty());
    }

    SECTION("is_dag")
    {
        auto id1 = graph.add_file_node(FileNode { .name = graph.intern("a") });
        auto id2 = graph.add_file_node(FileNode { .name = graph.intern("b") });

        (void)graph.add_edge(*id1, *id2);

        REQUIRE(is_dag(graph));

        (void)graph.add_edge(*id2, *id1);
        REQUIRE_FALSE(is_dag(graph));
    }
}

TEST_CASE("BuildGraph node types", "[graph]")
{
    auto graph = BuildGraph {};

    SECTION("Group node type")
    {
        auto node = FileNode { .type = NodeType::Group, .name = graph.intern("{objs}") };
        auto id = graph.add_file_node(node);

        REQUIRE(id.has_value());
        auto const* n = graph.get_file_node(*id);
        REQUIRE(n != nullptr);
        REQUIRE(n->type == NodeType::Group);
    }

    SECTION("all node types")
    {
        auto file_id = graph.add_file_node(FileNode { .name = graph.intern("a.c") });
        auto cmd_id = graph.add_command_node(CommandNode { .command = graph.intern("gcc") });
        auto gen_id = graph.add_file_node(FileNode { .type = NodeType::Generated, .name = graph.intern("a.o") });
        auto dir_id = graph.add_file_node(FileNode { .type = NodeType::Directory, .name = graph.intern("src") });
        auto var_id = graph.add_file_node(FileNode { .type = NodeType::Variable, .name = graph.intern("CC") });
        auto group_id = graph.add_file_node(FileNode { .type = NodeType::Group, .name = graph.intern("{objs}") });
        auto gen_dir_id = graph.add_file_node(FileNode { .type = NodeType::GeneratedDir, .name = graph.intern("build") });

        REQUIRE(file_id.has_value());
        REQUIRE(cmd_id.has_value());
        REQUIRE(gen_id.has_value());
        REQUIRE(dir_id.has_value());
        REQUIRE(var_id.has_value());
        REQUIRE(group_id.has_value());
        REQUIRE(gen_dir_id.has_value());

        // 7 nodes we added + BUILD_ROOT_ID = 8
        REQUIRE(graph.node_count() == 8);
        REQUIRE(graph.nodes_of_type(NodeType::File).size() == 1);
        REQUIRE(graph.nodes_of_type(NodeType::Command).size() == 1);
        REQUIRE(graph.nodes_of_type(NodeType::Generated).size() == 1);
        REQUIRE(graph.nodes_of_type(NodeType::Group).size() == 1);
    }
}

TEST_CASE("BuildGraph edge types", "[graph]")
{
    auto graph = BuildGraph {};

    SECTION("order-only edges")
    {
        auto id1 = graph.add_file_node(FileNode { .name = graph.intern("header.h") });
        auto id2 = graph.add_command_node(CommandNode { .command = graph.intern("gcc") });

        REQUIRE(id1.has_value());
        REQUIRE(id2.has_value());

        auto e = graph.add_order_only_edge(*id1, *id2);
        REQUIRE(e.has_value());

        auto order_inputs = graph.get_order_only(*id2);
        REQUIRE(order_inputs.size() == 1);
        REQUIRE(order_inputs[0] == *id1);
    }

    SECTION("group edges")
    {
        // Group edge: command -> group (command produces outputs in group)
        auto cmd_id = graph.add_command_node(CommandNode { .command = graph.intern("gcc") });
        auto out_id = graph.add_file_node(FileNode { .type = NodeType::Generated, .name = graph.intern("a.o") });
        auto group_id = graph.add_file_node(FileNode { .type = NodeType::Group, .name = graph.intern("{objs}") });

        REQUIRE(cmd_id.has_value());
        REQUIRE(out_id.has_value());
        REQUIRE(group_id.has_value());

        (void)graph.add_edge(*cmd_id, *out_id);

        auto edge_result = graph.add_edge(*out_id, *group_id, pup::LinkType::Group);
        REQUIRE(edge_result.has_value());
    }
}

TEST_CASE("Graph traversal", "[graph]")
{
    auto graph = BuildGraph {};

    //     a
    //    / |
    //   b   c
    //   |   |
    //   d   e
    auto id_a = graph.add_file_node(FileNode { .name = graph.intern("a") });
    auto id_b = graph.add_file_node(FileNode { .name = graph.intern("b") });
    auto id_c = graph.add_file_node(FileNode { .name = graph.intern("c") });
    auto id_d = graph.add_file_node(FileNode { .name = graph.intern("d") });
    auto id_e = graph.add_file_node(FileNode { .name = graph.intern("e") });

    (void)graph.add_edge(*id_a, *id_b);
    (void)graph.add_edge(*id_a, *id_c);
    (void)graph.add_edge(*id_b, *id_d);
    (void)graph.add_edge(*id_c, *id_e);

    SECTION("has_path")
    {
        REQUIRE(has_path(graph, *id_a, *id_d));
        REQUIRE(has_path(graph, *id_a, *id_e));
        REQUIRE_FALSE(has_path(graph, *id_d, *id_a));
        REQUIRE_FALSE(has_path(graph, *id_b, *id_c));
    }
}

TEST_CASE("Order-only dependencies in topological sort", "[graph]")
{
    auto graph = BuildGraph {};

    // Build a graph where:
    // - header.h is an order-only dependency of cmd
    // - input.c is a normal input to cmd
    // - cmd produces output.o
    //
    // header.h ---(order-only)---> cmd ---> output.o
    //                               ^
    // input.c ----------------------+

    auto header_id = graph.add_file_node(FileNode { .name = graph.intern("header.h") });
    auto input_id = graph.add_file_node(FileNode { .name = graph.intern("input.c") });
    auto cmd_id = graph.add_command_node(CommandNode { .command = graph.intern("gcc") });
    auto output_id = graph.add_file_node(FileNode { .type = NodeType::Generated, .name = graph.intern("output.o") });

    REQUIRE(header_id.has_value());
    REQUIRE(input_id.has_value());
    REQUIRE(cmd_id.has_value());
    REQUIRE(output_id.has_value());

    // Normal edge: input.c -> cmd
    (void)graph.add_edge(*input_id, *cmd_id);
    // Normal edge: cmd -> output.o
    (void)graph.add_edge(*cmd_id, *output_id);
    // Order-only edge: header.h -> cmd
    (void)graph.add_order_only_edge(*header_id, *cmd_id);

    SECTION("get_order_only_dependents returns correct nodes")
    {
        auto dependents = graph.get_order_only_dependents(*header_id);
        REQUIRE(dependents.size() == 1);
        REQUIRE(dependents[0] == *cmd_id);
    }

    SECTION("topological sort respects order-only dependencies")
    {
        auto result = topological_sort(graph);
        REQUIRE_FALSE(result.has_cycle);

        // Find positions in the sorted order
        auto find_pos = [&](pup::NodeId id) -> std::size_t {
            for (std::size_t i = 0; i < result.order.size(); ++i) {
                if (result.order[i] == id)
                    return i;
            }
            return result.order.size();
        };

        auto header_pos = find_pos(*header_id);
        auto input_pos = find_pos(*input_id);
        auto cmd_pos = find_pos(*cmd_id);
        auto output_pos = find_pos(*output_id);

        // header.h must come before cmd (order-only constraint)
        REQUIRE(header_pos < cmd_pos);
        // input.c must come before cmd (normal dependency)
        REQUIRE(input_pos < cmd_pos);
        // cmd must come before output.o (normal dependency)
        REQUIRE(cmd_pos < output_pos);
    }

    SECTION("cycle detection includes order-only edges")
    {
        // Create a cycle via order-only: output.o --order-only--> header.h
        // This creates: header.h -> cmd -> output.o -> header.h (cycle)
        (void)graph.add_order_only_edge(*output_id, *header_id);

        auto result = topological_sort(graph);
        REQUIRE(result.has_cycle);
    }
}

// Regression test: get_outputs() must exclude sticky edges
//
// Background: Sticky edges connect Tupfile/config nodes to commands they define.
// These are parse-time dependencies (if Tupfile changes, reparse to see if command
// changed), NOT build-time dependencies (don't rebuild command just because Tupfile
// was touched).
//
// Bug: Previously get_outputs() returned ALL edges including sticky. During
// incremental builds, the expansion loop followed get_outputs() transitively,
// causing a cascade through shared Tupfile nodes to ALL commands.
//
// Fix: Separate storage - sticky edges go to sticky_outputs, get_outputs()
// returns only non-sticky edges (matching tup's design).
//
// This test would FAIL with the old implementation where get_outputs()
// included sticky edges.
TEST_CASE("get_outputs excludes sticky edges", "[graph][regression]")
{
    using pup::LinkType;
    auto graph = BuildGraph {};

    // Scenario: Tupfile defines a command, source.c is input
    //
    // Tupfile ---(sticky)---> cmd ---(normal)---> output.o
    //                          ^
    // source.c ---(normal)-----+

    auto tupfile_id = graph.add_file_node(FileNode { .name = graph.intern("Tupfile") });
    auto source_id = graph.add_file_node(FileNode { .name = graph.intern("source.c") });
    auto cmd_id = graph.add_command_node(CommandNode { .command = graph.intern("gcc source.c") });
    auto output_id = graph.add_file_node(FileNode { .type = NodeType::Generated, .name = graph.intern("output.o") });

    (void)graph.add_edge(*source_id, *cmd_id, LinkType::Normal);
    (void)graph.add_edge(*cmd_id, *output_id, LinkType::Normal);
    (void)graph.add_edge(*tupfile_id, *cmd_id, LinkType::Sticky);

    // THE KEY ASSERTION: Tupfile's get_outputs() must NOT include the command
    // Old behavior: tupfile_outputs contained cmd_id (WRONG - causes cascade)
    // New behavior: tupfile_outputs is empty (CORRECT - sticky edges excluded)
    auto tupfile_outputs = graph.get_outputs(*tupfile_id);
    REQUIRE(tupfile_outputs.empty());

    // Normal edges still work as expected
    auto source_outputs = graph.get_outputs(*source_id);
    REQUIRE(source_outputs.size() == 1);
    REQUIRE(source_outputs[0] == *cmd_id);

    auto cmd_outputs = graph.get_outputs(*cmd_id);
    REQUIRE(cmd_outputs.size() == 1);
    REQUIRE(cmd_outputs[0] == *output_id);
}

TEST_CASE("Sticky edge API", "[graph]")
{
    using pup::LinkType;
    auto graph = BuildGraph {};

    auto tupfile_id = graph.add_file_node(FileNode { .name = graph.intern("Tupfile") });
    auto source_id = graph.add_file_node(FileNode { .name = graph.intern("source.c") });
    auto cmd_id = graph.add_command_node(CommandNode { .command = graph.intern("gcc source.c") });
    auto output_id = graph.add_file_node(FileNode { .type = NodeType::Generated, .name = graph.intern("output.o") });

    (void)graph.add_edge(*source_id, *cmd_id, LinkType::Normal);
    (void)graph.add_edge(*cmd_id, *output_id, LinkType::Normal);
    (void)graph.add_edge(*tupfile_id, *cmd_id, LinkType::Sticky);

    SECTION("get_sticky_outputs returns only sticky edges")
    {
        auto tupfile_sticky = graph.get_sticky_outputs(*tupfile_id);
        REQUIRE(tupfile_sticky.size() == 1);
        REQUIRE(tupfile_sticky[0] == *cmd_id);

        REQUIRE(graph.get_sticky_outputs(*source_id).empty());
        REQUIRE(graph.get_sticky_outputs(*cmd_id).empty());
    }

    SECTION("inputs include all edge types")
    {
        auto cmd_inputs = graph.get_inputs(*cmd_id);
        REQUIRE(cmd_inputs.size() == 2);
        REQUIRE(std::ranges::find(cmd_inputs, *tupfile_id) != cmd_inputs.end());
        REQUIRE(std::ranges::find(cmd_inputs, *source_id) != cmd_inputs.end());
    }

    SECTION("edges() returns all edges including sticky")
    {
        REQUIRE(graph.edge_count() == 3);

        auto edges = graph.edges();
        auto sticky_edge = std::ranges::find_if(edges, [](auto const& e) {
            return e.type == LinkType::Sticky;
        });
        REQUIRE(sticky_edge != edges.end());
        REQUIRE(sticky_edge->from == *tupfile_id);
        REQUIRE(sticky_edge->to == *cmd_id);
    }

    SECTION("multiple sticky edges from same node")
    {
        auto cmd2_id = graph.add_command_node(CommandNode { .command = graph.intern("gcc other.c") });
        (void)graph.add_edge(*tupfile_id, *cmd2_id, LinkType::Sticky);

        REQUIRE(graph.get_sticky_outputs(*tupfile_id).size() == 2);
        REQUIRE(graph.get_outputs(*tupfile_id).empty());
    }

    SECTION("mixed edge types from same node")
    {
        auto cmd2_id = graph.add_command_node(CommandNode { .command = graph.intern("lint source.c") });
        (void)graph.add_edge(*source_id, *cmd2_id, LinkType::Sticky);

        auto source_outputs = graph.get_outputs(*source_id);
        REQUIRE(source_outputs.size() == 1);
        REQUIRE(source_outputs[0] == *cmd_id);

        auto source_sticky = graph.get_sticky_outputs(*source_id);
        REQUIRE(source_sticky.size() == 1);
        REQUIRE(source_sticky[0] == *cmd2_id);
    }
}
