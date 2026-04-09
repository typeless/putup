// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/cli/config_commands.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/topo.hpp"

#include <algorithm>

using namespace pup::graph;
using pup::NodeType;

namespace {
auto sv(pup::StringId id) -> std::string_view { return pup::global_pool().get(id); }
auto intern(std::string_view s) -> pup::StringId { return pup::global_pool().intern(s); }
} // namespace

TEST_CASE("BuildGraph basic operations", "[graph]")
{
    auto bs = make_build_state();
    auto& g = bs.graph;

    SECTION("add nodes")
    {
        auto node1 = FileNode { .name = intern("foo.c") };
        auto node2 = FileNode { .name = intern("bar.c") };

        auto id1 = add_file_node(g, node1);
        auto id2 = add_file_node(g, node2);

        REQUIRE(id1.has_value());
        REQUIRE(id2.has_value());
        REQUIRE(*id1 != *id2);

        // Graph starts with BUILD_ROOT_ID (id=1), so after adding 2 nodes, count is 3
        REQUIRE(node_count(g) == 3);
    }

    SECTION("find by dir and name")
    {
        // Create directory node first
        auto dir_node = FileNode { .type = NodeType::Directory, .name = intern("src") };
        auto dir_id = add_file_node(g, dir_node);
        REQUIRE(dir_id.has_value());

        // Create file node with parent_dir and name
        auto file_node = FileNode {
            .name = intern("foo.c"),
            .parent_dir = *dir_id,
        };
        auto file_id = add_file_node(g, file_node);
        REQUIRE(file_id.has_value());

        // Find by (parent_dir, name)
        auto found = find_by_dir_name(g, *dir_id, "foo.c");
        REQUIRE(found.has_value());
        REQUIRE(*found == *file_id);

        // Not found cases
        REQUIRE_FALSE(find_by_dir_name(g, *dir_id, "bar.c").has_value());
        REQUIRE_FALSE(find_by_dir_name(g, 0, "foo.c").has_value());
    }

    SECTION("find by dir and name - multiple directories")
    {
        // src/ and lib/ directories
        auto src_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("src") });
        auto lib_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("lib") });
        REQUIRE(src_dir.has_value());
        REQUIRE(lib_dir.has_value());

        // Same basename in different directories
        auto src_file = add_file_node(g, FileNode {
                                             .name = intern("util.c"),
                                             .parent_dir = *src_dir,
                                         });
        auto lib_file = add_file_node(g, FileNode {
                                             .name = intern("util.c"),
                                             .parent_dir = *lib_dir,
                                         });
        REQUIRE(src_file.has_value());
        REQUIRE(lib_file.has_value());

        // Each lookup returns the correct node
        auto found_src = find_by_dir_name(g, *src_dir, "util.c");
        auto found_lib = find_by_dir_name(g, *lib_dir, "util.c");
        REQUIRE(found_src.has_value());
        REQUIRE(found_lib.has_value());
        REQUIRE(*found_src == *src_file);
        REQUIRE(*found_lib == *lib_file);
        REQUIRE(*found_src != *found_lib);
    }

    SECTION("get_full_path - simple hierarchy")
    {
        // Build: src/foo.c
        auto src_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("src") });
        auto foo_file = add_file_node(g, FileNode {
                                             .name = intern("foo.c"),
                                             .parent_dir = *src_dir,
                                         });
        REQUIRE(src_dir.has_value());
        REQUIRE(foo_file.has_value());

        REQUIRE(sv(get_full_path(g, *src_dir)) == "src");
        REQUIRE(sv(get_full_path(g, *foo_file)) == "src/foo.c");
    }

    SECTION("get_full_path - deep hierarchy")
    {
        // Build: a/b/c/d/file.txt
        auto a_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("a") });
        auto b_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("b"), .parent_dir = *a_dir });
        auto c_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("c"), .parent_dir = *b_dir });
        auto d_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("d"), .parent_dir = *c_dir });
        auto file = add_file_node(g, FileNode { .name = intern("file.txt"), .parent_dir = *d_dir });

        REQUIRE(sv(get_full_path(g, *a_dir)) == "a");
        REQUIRE(sv(get_full_path(g, *b_dir)) == "a/b");
        REQUIRE(sv(get_full_path(g, *c_dir)) == "a/b/c");
        REQUIRE(sv(get_full_path(g, *d_dir)) == "a/b/c/d");
        REQUIRE(sv(get_full_path(g, *file)) == "a/b/c/d/file.txt");
    }

    SECTION("get_full_path - name with path separator (no parent_dir)")
    {
        auto node = add_file_node(g, FileNode { .name = intern("legacy/path.c") });
        REQUIRE(sv(get_full_path(g, *node)) == "legacy/path.c");
    }

    SECTION("get_full_path - root level file")
    {
        auto file = add_file_node(g, FileNode { .name = intern("Makefile") });
        REQUIRE(sv(get_full_path(g, *file)) == "Makefile");
    }

    SECTION("get_full_path - invalid node returns empty")
    {
        REQUIRE(sv(get_full_path(g, 0)) == "");
        REQUIRE(sv(get_full_path(g, 9999)) == "");
    }

    SECTION("get_full_path - caching works")
    {
        auto dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("cached") });
        auto file = add_file_node(g, FileNode { .name = intern("test.c"), .parent_dir = *dir });

        // First call computes and caches
        auto path1 = get_full_path(g, *file, bs.path_cache);
        // Second call should hit cache
        auto path2 = get_full_path(g, *file, bs.path_cache);

        REQUIRE(path1 == "cached/test.c");
        REQUIRE(path2 == "cached/test.c");
    }

    SECTION("invalidate_path_cache and clear_path_cache")
    {
        auto dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("dir") });
        auto file = add_file_node(g, FileNode { .name = intern("file.c"), .parent_dir = *dir });

        // Populate cache
        (void)get_full_path(g, *file, bs.path_cache);
        (void)get_full_path(g, *dir, bs.path_cache);

        // Invalidate single entry
        invalidate_path_cache(bs.path_cache, *file);
        // Should still work (recomputes)
        REQUIRE(get_full_path(g, *file, bs.path_cache) == "dir/file.c");

        // Clear entire cache
        clear_path_cache(bs.path_cache);
        // Should still work (recomputes)
        REQUIRE(get_full_path(g, *dir, bs.path_cache) == "dir");
        REQUIRE(get_full_path(g, *file, bs.path_cache) == "dir/file.c");
    }

    SECTION("add edges")
    {
        auto id1 = add_file_node(g, FileNode { .name = intern("foo.c") });
        auto id2 = add_command_node(g, CommandNode { .instruction_id = intern("gcc foo.c") });
        auto id3 = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("foo.o") });

        REQUIRE(id1.has_value());
        REQUIRE(id2.has_value());
        REQUIRE(id3.has_value());

        auto e1 = add_edge(g, *id1, *id2);
        auto e2 = add_edge(g, *id2, *id3);

        REQUIRE(e1.has_value());
        REQUIRE(e2.has_value());

        REQUIRE(edge_count(g) == 2);

        auto inputs = get_inputs(g, *id2);
        REQUIRE(inputs.size() == 1);
        REQUIRE(inputs[0] == *id1);

        auto outputs = get_outputs(g, *id2);
        REQUIRE(outputs.size() == 1);
        REQUIRE(outputs[0] == *id3);
    }

    SECTION("nodes of type")
    {
        (void)add_file_node(g, FileNode { .name = intern("a.c") });
        (void)add_file_node(g, FileNode { .name = intern("b.c") });
        (void)add_command_node(g, CommandNode { .instruction_id = intern("gcc") });
        (void)add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("out.o") });

        auto files = nodes_of_type(g, NodeType::File);
        REQUIRE(files.size() == 2);

        auto commands = nodes_of_type(g, NodeType::Command);
        REQUIRE(commands.size() == 1);

        auto generated = nodes_of_type(g, NodeType::Generated);
        REQUIRE(generated.size() == 1);
    }

    SECTION("root and leaf nodes")
    {
        auto id1 = add_file_node(g, FileNode { .name = intern("a.c") });
        auto id2 = add_command_node(g, CommandNode { .instruction_id = intern("") });
        auto id3 = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("a.o") });

        (void)add_edge(g, *id1, *id2);
        (void)add_edge(g, *id2, *id3);

        auto roots = root_nodes(g);
        // Root nodes include BUILD_ROOT_ID (which has no inputs) plus a.c
        REQUIRE(roots.size() == 2);
        // One of the roots should be the file we added
        REQUIRE(std::ranges::find(roots, *id1) != roots.end());

        auto leaves = leaf_nodes(g);
        // BUILD_ROOT_ID is also a leaf (no outputs unless used)
        REQUIRE(leaves.size() == 2);
        REQUIRE(std::ranges::find(leaves, *id3) != leaves.end());
    }
}

TEST_CASE("Topological sort", "[graph]")
{
    auto bs = make_build_state();
    auto& g = bs.graph;

    SECTION("simple linear graph")
    {
        auto id1 = add_file_node(g, FileNode { .name = intern("a") });
        auto id2 = add_file_node(g, FileNode { .name = intern("b") });
        auto id3 = add_file_node(g, FileNode { .name = intern("c") });

        (void)add_edge(g, *id1, *id2);
        (void)add_edge(g, *id2, *id3);

        auto result = topological_sort(g);
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
        auto id_a = add_file_node(g, FileNode { .name = intern("a") });
        auto id_b = add_file_node(g, FileNode { .name = intern("b") });
        auto id_c = add_file_node(g, FileNode { .name = intern("c") });
        auto id_d = add_file_node(g, FileNode { .name = intern("d") });

        (void)add_edge(g, *id_a, *id_b);
        (void)add_edge(g, *id_a, *id_c);
        (void)add_edge(g, *id_b, *id_d);
        (void)add_edge(g, *id_c, *id_d);

        auto result = topological_sort(g);
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
        auto id1 = add_file_node(g, FileNode { .name = intern("a") });
        auto id2 = add_file_node(g, FileNode { .name = intern("b") });
        auto id3 = add_file_node(g, FileNode { .name = intern("c") });

        (void)add_edge(g, *id1, *id2);
        (void)add_edge(g, *id2, *id3);
        (void)add_edge(g, *id3, *id1); // Creates cycle

        auto result = topological_sort(g);
        REQUIRE(result.has_cycle);
        REQUIRE_FALSE(result.cycle.empty());
    }

    SECTION("is_dag")
    {
        auto id1 = add_file_node(g, FileNode { .name = intern("a") });
        auto id2 = add_file_node(g, FileNode { .name = intern("b") });

        (void)add_edge(g, *id1, *id2);

        REQUIRE(is_dag(g));

        (void)add_edge(g, *id2, *id1);
        REQUIRE_FALSE(is_dag(g));
    }
}

TEST_CASE("BuildGraph node types", "[graph]")
{
    auto bs = make_build_state();
    auto& g = bs.graph;

    SECTION("Group node type")
    {
        auto node = FileNode { .type = NodeType::Group, .name = intern("{objs}") };
        auto id = add_file_node(g, node);

        REQUIRE(id.has_value());
        auto const* n = get_file_node(g, *id);
        REQUIRE(n != nullptr);
        REQUIRE(n->type == NodeType::Group);
    }

    SECTION("all node types")
    {
        auto file_id = add_file_node(g, FileNode { .name = intern("a.c") });
        auto cmd_id = add_command_node(g, CommandNode { .instruction_id = intern("gcc") });
        auto gen_id = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("a.o") });
        auto dir_id = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("src") });
        auto var_id = add_file_node(g, FileNode { .type = NodeType::Variable, .name = intern("CC") });
        auto group_id = add_file_node(g, FileNode { .type = NodeType::Group, .name = intern("{objs}") });
        auto gen_dir_id = add_file_node(g, FileNode { .type = NodeType::GeneratedDir, .name = intern("build") });

        REQUIRE(file_id.has_value());
        REQUIRE(cmd_id.has_value());
        REQUIRE(gen_id.has_value());
        REQUIRE(dir_id.has_value());
        REQUIRE(var_id.has_value());
        REQUIRE(group_id.has_value());
        REQUIRE(gen_dir_id.has_value());

        // 7 nodes we added + BUILD_ROOT_ID = 8
        REQUIRE(node_count(g) == 8);
        REQUIRE(nodes_of_type(g, NodeType::File).size() == 1);
        REQUIRE(nodes_of_type(g, NodeType::Command).size() == 1);
        REQUIRE(nodes_of_type(g, NodeType::Generated).size() == 1);
        REQUIRE(nodes_of_type(g, NodeType::Group).size() == 1);
    }
}

TEST_CASE("BuildGraph edge types", "[graph]")
{
    auto bs = make_build_state();
    auto& g = bs.graph;

    SECTION("order-only edges")
    {
        auto id1 = add_file_node(g, FileNode { .name = intern("header.h") });
        auto id2 = add_command_node(g, CommandNode { .instruction_id = intern("gcc") });

        REQUIRE(id1.has_value());
        REQUIRE(id2.has_value());

        auto e = add_order_only_edge(g, *id1, *id2);
        REQUIRE(e.has_value());

        auto order_inputs = get_order_only(g, *id2);
        REQUIRE(order_inputs.size() == 1);
        REQUIRE(order_inputs[0] == *id1);
    }

    SECTION("group edges")
    {
        // Group edge: command -> group (command produces outputs in group)
        auto cmd_id = add_command_node(g, CommandNode { .instruction_id = intern("gcc") });
        auto out_id = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("a.o") });
        auto group_id = add_file_node(g, FileNode { .type = NodeType::Group, .name = intern("{objs}") });

        REQUIRE(cmd_id.has_value());
        REQUIRE(out_id.has_value());
        REQUIRE(group_id.has_value());

        (void)add_edge(g, *cmd_id, *out_id);

        auto edge_result = add_edge(g, *out_id, *group_id, pup::LinkType::Group);
        REQUIRE(edge_result.has_value());
    }
}

TEST_CASE("Graph traversal", "[graph]")
{
    auto bs = make_build_state();
    auto& g = bs.graph;

    //     a
    //    / |
    //   b   c
    //   |   |
    //   d   e
    auto id_a = add_file_node(g, FileNode { .name = intern("a") });
    auto id_b = add_file_node(g, FileNode { .name = intern("b") });
    auto id_c = add_file_node(g, FileNode { .name = intern("c") });
    auto id_d = add_file_node(g, FileNode { .name = intern("d") });
    auto id_e = add_file_node(g, FileNode { .name = intern("e") });

    (void)add_edge(g, *id_a, *id_b);
    (void)add_edge(g, *id_a, *id_c);
    (void)add_edge(g, *id_b, *id_d);
    (void)add_edge(g, *id_c, *id_e);

    SECTION("has_path")
    {
        REQUIRE(has_path(g, *id_a, *id_d));
        REQUIRE(has_path(g, *id_a, *id_e));
        REQUIRE_FALSE(has_path(g, *id_d, *id_a));
        REQUIRE_FALSE(has_path(g, *id_b, *id_c));
    }
}

TEST_CASE("Order-only dependencies in topological sort", "[graph]")
{
    auto bs = make_build_state();
    auto& g = bs.graph;

    // Build a graph where:
    // - header.h is an order-only dependency of cmd
    // - input.c is a normal input to cmd
    // - cmd produces output.o
    //
    // header.h ---(order-only)---> cmd ---> output.o
    //                               ^
    // input.c ----------------------+

    auto header_id = add_file_node(g, FileNode { .name = intern("header.h") });
    auto input_id = add_file_node(g, FileNode { .name = intern("input.c") });
    auto cmd_id = add_command_node(g, CommandNode { .instruction_id = intern("gcc") });
    auto output_id = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("output.o") });

    REQUIRE(header_id.has_value());
    REQUIRE(input_id.has_value());
    REQUIRE(cmd_id.has_value());
    REQUIRE(output_id.has_value());

    // Normal edge: input.c -> cmd
    (void)add_edge(g, *input_id, *cmd_id);
    // Normal edge: cmd -> output.o
    (void)add_edge(g, *cmd_id, *output_id);
    // Order-only edge: header.h -> cmd
    (void)add_order_only_edge(g, *header_id, *cmd_id);

    SECTION("get_order_only_dependents returns correct nodes")
    {
        auto dependents = get_order_only_dependents(g, *header_id);
        REQUIRE(dependents.size() == 1);
        REQUIRE(dependents[0] == *cmd_id);
    }

    SECTION("topological sort respects order-only dependencies")
    {
        auto result = topological_sort(g);
        REQUIRE_FALSE(result.has_cycle);

        // Find positions in the sorted order
        auto find_pos = [&](pup::NodeId id) -> std::size_t {
            for (std::size_t i = 0; i < result.order.size(); ++i) {
                if (result.order[i] == id) {
                    return i;
                }
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
        (void)add_order_only_edge(g, *output_id, *header_id);

        auto result = topological_sort(g);
        REQUIRE(result.has_cycle);
    }
}

TEST_CASE("Unified edge storage for order-only edges", "[graph]")
{
    auto bs = make_build_state();
    auto& g = bs.graph;

    // group1 ---(order-only)---> cmd ---(normal)---> output.o
    //                             ^
    // input.c ----(normal)-------+

    auto group_id = add_file_node(g, FileNode { .type = NodeType::Group, .name = intern("<libs>") });
    auto input_id = add_file_node(g, FileNode { .name = intern("input.c") });
    auto cmd_id = add_command_node(g, CommandNode { .instruction_id = intern("gcc") });
    auto output_id = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("output.o") });

    REQUIRE(group_id.has_value());
    REQUIRE(input_id.has_value());
    REQUIRE(cmd_id.has_value());
    REQUIRE(output_id.has_value());

    (void)add_edge(g, *input_id, *cmd_id);
    (void)add_edge(g, *cmd_id, *output_id);
    (void)add_order_only_edge(g, *group_id, *cmd_id);

    SECTION("order-only edges are counted in edge_count")
    {
        REQUIRE(edge_count(g) == 3);
    }

    SECTION("get_inputs excludes order-only sources")
    {
        auto inputs = get_inputs(g, *cmd_id);
        REQUIRE(inputs.size() == 1);
        REQUIRE(inputs[0] == *input_id);
    }

    SECTION("get_outputs excludes order-only targets")
    {
        auto outputs = get_outputs(g, *group_id);
        REQUIRE(outputs.empty());
    }

    SECTION("get_order_only returns correct sources via unified index")
    {
        auto oo = get_order_only(g, *cmd_id);
        REQUIRE(oo.size() == 1);
        REQUIRE(oo[0] == *group_id);
    }

    SECTION("get_order_only_dependents returns correct targets via unified index")
    {
        auto deps = get_order_only_dependents(g, *group_id);
        REQUIRE(deps.size() == 1);
        REQUIRE(deps[0] == *cmd_id);
    }

    SECTION("root_nodes excludes nodes with only order-only inputs")
    {
        auto roots = root_nodes(g);
        // cmd has inputs (normal + order-only), so it's not a root
        REQUIRE(std::ranges::find(roots, *cmd_id) == roots.end());
        // group1 has no inputs, so it IS a root
        REQUIRE(std::ranges::find(roots, *group_id) != roots.end());
        // input.c has no inputs, so it IS a root
        REQUIRE(std::ranges::find(roots, *input_id) != roots.end());
    }

    SECTION("leaf_nodes excludes nodes with only order-only forward edges")
    {
        // group1 has only an order-only forward edge to cmd.
        // Before unification this was stored separately and leaf_nodes missed it,
        // so group1 incorrectly appeared as a leaf. After unification, group1
        // should NOT be a leaf because it has a forward edge (order-only counts).
        auto leaves = leaf_nodes(g);
        REQUIRE(std::ranges::find(leaves, *group_id) == leaves.end());
    }

    SECTION("root_nodes excludes command with only order-only input")
    {
        auto bs2 = make_build_state();
        auto& g2 = bs2.graph;

        auto grp = add_file_node(g2, FileNode { .type = NodeType::Group, .name = intern("<order>") });
        auto cmd = add_command_node(g2, CommandNode { .instruction_id = intern("touch") });
        auto out = add_file_node(g2, FileNode { .type = NodeType::Generated, .name = intern("stamp") });

        REQUIRE(grp.has_value());
        REQUIRE(cmd.has_value());
        REQUIRE(out.has_value());

        (void)add_order_only_edge(g2, *grp, *cmd);
        (void)add_edge(g2, *cmd, *out);

        auto roots = root_nodes(g2);
        REQUIRE(std::ranges::find(roots, *cmd) == roots.end());
        REQUIRE(std::ranges::find(roots, *grp) != roots.end());
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
// Fix: get_outputs() filters out LinkType::Sticky and LinkType::OrderOnly edges,
// returning only data-flow edges (matching tup's design).
//
// This test would FAIL with the old implementation where get_outputs()
// included sticky edges.
TEST_CASE("get_outputs excludes sticky edges", "[graph][regression]")
{
    using pup::LinkType;
    auto bs = make_build_state();
    auto& g = bs.graph;

    // Scenario: Tupfile defines a command, source.c is input
    //
    // Tupfile ---(sticky)---> cmd ---(normal)---> output.o
    //                          ^
    // source.c ---(normal)-----+

    auto tupfile_id = add_file_node(g, FileNode { .name = intern("Tupfile") });
    auto source_id = add_file_node(g, FileNode { .name = intern("source.c") });
    auto cmd_id = add_command_node(g, CommandNode { .instruction_id = intern("gcc source.c") });
    auto output_id = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("output.o") });

    (void)add_edge(g, *source_id, *cmd_id, LinkType::Normal);
    (void)add_edge(g, *cmd_id, *output_id, LinkType::Normal);
    (void)add_edge(g, *tupfile_id, *cmd_id, LinkType::Sticky);

    // THE KEY ASSERTION: Tupfile's get_outputs() must NOT include the command
    // Old behavior: tupfile_outputs contained cmd_id (WRONG - causes cascade)
    // New behavior: tupfile_outputs is empty (CORRECT - sticky edges excluded)
    auto tupfile_outputs = get_outputs(g, *tupfile_id);
    REQUIRE(tupfile_outputs.empty());

    // Normal edges still work as expected
    auto source_outputs = get_outputs(g, *source_id);
    REQUIRE(source_outputs.size() == 1);
    REQUIRE(source_outputs[0] == *cmd_id);

    auto cmd_outputs = get_outputs(g, *cmd_id);
    REQUIRE(cmd_outputs.size() == 1);
    REQUIRE(cmd_outputs[0] == *output_id);
}

TEST_CASE("Sticky edge API", "[graph]")
{
    using pup::LinkType;
    auto bs = make_build_state();
    auto& g = bs.graph;

    auto tupfile_id = add_file_node(g, FileNode { .name = intern("Tupfile") });
    auto source_id = add_file_node(g, FileNode { .name = intern("source.c") });
    auto cmd_id = add_command_node(g, CommandNode { .instruction_id = intern("gcc source.c") });
    auto output_id = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("output.o") });

    (void)add_edge(g, *source_id, *cmd_id, LinkType::Normal);
    (void)add_edge(g, *cmd_id, *output_id, LinkType::Normal);
    (void)add_edge(g, *tupfile_id, *cmd_id, LinkType::Sticky);

    SECTION("get_sticky_outputs returns only sticky edges")
    {
        auto tupfile_sticky = get_sticky_outputs(g, *tupfile_id);
        REQUIRE(tupfile_sticky.size() == 1);
        REQUIRE(tupfile_sticky[0] == *cmd_id);

        REQUIRE(get_sticky_outputs(g, *source_id).empty());
        REQUIRE(get_sticky_outputs(g, *cmd_id).empty());
    }

    SECTION("inputs include all edge types")
    {
        auto cmd_inputs = get_inputs(g, *cmd_id);
        REQUIRE(cmd_inputs.size() == 2);
        REQUIRE(std::ranges::find(cmd_inputs, *tupfile_id) != cmd_inputs.end());
        REQUIRE(std::ranges::find(cmd_inputs, *source_id) != cmd_inputs.end());
    }

    SECTION("edges() returns all edges including sticky")
    {
        REQUIRE(edge_count(g) == 3);

        auto edges = g.edges;
        auto sticky_edge = std::ranges::find_if(edges, [](auto const& e) {
            return e.type == LinkType::Sticky;
        });
        REQUIRE(sticky_edge != edges.end());
        REQUIRE(sticky_edge->from == *tupfile_id);
        REQUIRE(sticky_edge->to == *cmd_id);
    }

    SECTION("multiple sticky edges from same node")
    {
        auto cmd2_id = add_command_node(g, CommandNode { .instruction_id = intern("gcc other.c") });
        (void)add_edge(g, *tupfile_id, *cmd2_id, LinkType::Sticky);

        REQUIRE(get_sticky_outputs(g, *tupfile_id).size() == 2);
        REQUIRE(get_outputs(g, *tupfile_id).empty());
    }

    SECTION("mixed edge types from same node")
    {
        auto cmd2_id = add_command_node(g, CommandNode { .instruction_id = intern("lint source.c") });
        (void)add_edge(g, *source_id, *cmd2_id, LinkType::Sticky);

        auto source_outputs = get_outputs(g, *source_id);
        REQUIRE(source_outputs.size() == 1);
        REQUIRE(source_outputs[0] == *cmd_id);

        auto source_sticky = get_sticky_outputs(g, *source_id);
        REQUIRE(source_sticky.size() == 1);
        REQUIRE(source_sticky[0] == *cmd2_id);
    }
}

TEST_CASE("Template tracking via StringId deduplication", "[graph][template]")
{
    SECTION("same template pattern interns to same StringId")
    {
        auto t1 = intern("gcc -O2 -c -o %o %f");
        auto t2 = intern("gcc -O2 -c -o %o %f");
        REQUIRE(t1 == t2);
    }

    SECTION("different patterns intern to different StringIds")
    {
        auto t1 = intern("gcc -O2 -c -o %o %f");
        auto t2 = intern("g++ -O2 -c -o %o %f");
        REQUIRE(t1 != t2);
    }

    SECTION("empty template is valid")
    {
        auto empty = intern("");
        REQUIRE(empty == pup::StringId::Empty);
    }
}

TEST_CASE("CommandNode stores instruction_id", "[graph][instruction]")
{
    auto bs = make_build_state();
    auto& g = bs.graph;

    SECTION("command node with instruction_id")
    {
        auto instruction = intern("gcc -O2 -c -o %o %f");
        auto node = CommandNode {
            .display = intern("CC foo.o"),
            .instruction_id = instruction,
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto const* cmd = get_command_node(g, *cmd_id);
        REQUIRE(cmd != nullptr);
        REQUIRE(cmd->instruction_id == instruction);
        REQUIRE(get_instruction_pattern(g, *cmd_id) == "gcc -O2 -c -o %o %f");
    }

    SECTION("command node with literal instruction (no patterns)")
    {
        auto node = CommandNode {
            .instruction_id = intern("cp foo bar"),
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto const* cmd = get_command_node(g, *cmd_id);
        REQUIRE(cmd != nullptr);
        REQUIRE(cmd->instruction_id != pup::StringId::Empty);
        REQUIRE(get_instruction_pattern(g, *cmd_id) == "cp foo bar");
        REQUIRE(sv(expand_instruction(g, *cmd_id)) == "cp foo bar");
    }

    SECTION("command node with empty instruction_id")
    {
        auto node = CommandNode {};
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto const* cmd = get_command_node(g, *cmd_id);
        REQUIRE(cmd != nullptr);
        REQUIRE(cmd->instruction_id == pup::StringId::Empty);
        REQUIRE(get_instruction_pattern(g, *cmd_id).empty());
    }

    SECTION("multiple commands share same instruction")
    {
        auto instruction = intern("gcc -c -o %o %f");

        auto node1 = CommandNode {
            .instruction_id = instruction,
        };
        auto node2 = CommandNode {
            .instruction_id = instruction,
        };

        auto cmd1_id = add_command_node(g, std::move(node1));
        auto cmd2_id = add_command_node(g, std::move(node2));

        REQUIRE(cmd1_id.has_value());
        REQUIRE(cmd2_id.has_value());

        auto const* cmd1 = get_command_node(g, *cmd1_id);
        auto const* cmd2 = get_command_node(g, *cmd2_id);

        REQUIRE(cmd1->instruction_id == cmd2->instruction_id);
        REQUIRE(get_instruction_pattern(g, *cmd1_id) == get_instruction_pattern(g, *cmd2_id));
    }
}

TEST_CASE("expand_instruction reconstructs command from operands", "[graph][instruction]")
{
    auto bs = make_build_state();
    auto& g = bs.graph;

    // Build directory hierarchy: src/foo.c, src/bar.c, src/foo.o
    auto src_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("src") });
    REQUIRE(src_dir.has_value());

    auto foo_c = add_file_node(g, FileNode {
                                      .name = intern("foo.c"),
                                      .parent_dir = *src_dir,
                                  });
    auto bar_c = add_file_node(g, FileNode {
                                      .name = intern("bar.c"),
                                      .parent_dir = *src_dir,
                                  });
    auto foo_o = add_file_node(g, FileNode {
                                      .type = NodeType::Generated,
                                      .name = intern("foo.o"),
                                      .parent_dir = *src_dir,
                                  });
    REQUIRE(foo_c.has_value());
    REQUIRE(bar_c.has_value());
    REQUIRE(foo_o.has_value());

    SECTION("%f and %o with source_dir")
    {
        auto node = CommandNode {
            .source_dir = intern("src"),
            .instruction_id = intern("gcc -c %f -o %o"),
            .inputs = { *foo_c },
            .outputs = { *foo_o },
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "gcc -c foo.c -o foo.o");
    }

    SECTION("%f with multiple inputs")
    {
        auto node = CommandNode {
            .source_dir = intern("src"),
            .instruction_id = intern("gcc -c %f -o %o"),
            .inputs = { *foo_c, *bar_c },
            .outputs = { *foo_o },
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "gcc -c foo.c bar.c -o foo.o");
    }

    SECTION("%b (basename of first input)")
    {
        auto node = CommandNode {
            .source_dir = intern("src"),
            .instruction_id = intern("echo %b"),
            .inputs = { *foo_c },
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "echo foo.c");
    }

    SECTION("%B (stem of first input)")
    {
        auto node = CommandNode {
            .source_dir = intern("src"),
            .instruction_id = intern("echo %B"),
            .inputs = { *foo_c },
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "echo foo");
    }

    SECTION("%e (extension of first input)")
    {
        auto node = CommandNode {
            .source_dir = intern("src"),
            .instruction_id = intern("echo %e"),
            .inputs = { *foo_c },
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "echo c");
    }

    SECTION("%d (Tupfile directory name)")
    {
        // %d expands to the lowest-level directory name of the Tupfile (source_dir)
        // For source_dir="src", %d should be "src"
        auto node = CommandNode {
            .source_dir = intern("src"),
            .instruction_id = intern("echo %d"),
            .inputs = { *foo_c },
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "echo src");
    }

    SECTION("%d with nested source_dir")
    {
        // For source_dir="foo/bar/baz", %d should be "baz" (last component)
        auto node = CommandNode {
            .source_dir = intern("foo/bar/baz"),
            .instruction_id = intern("echo %d"),
            .inputs = { *foo_c },
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "echo baz");
    }

    SECTION("%O (basename of first output)")
    {
        auto node = CommandNode {
            .source_dir = intern("src"),
            .instruction_id = intern("echo %O"),
            .outputs = { *foo_o },
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "echo foo.o");
    }

    SECTION("%Nf (N-th input)")
    {
        auto node = CommandNode {
            .source_dir = intern("src"),
            .instruction_id = intern("gcc %1f %2f"),
            .inputs = { *foo_c, *bar_c },
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "gcc foo.c bar.c");
    }

    SECTION("%No (N-th output)")
    {
        auto bar_o = add_file_node(g, FileNode {
                                          .type = NodeType::Generated,
                                          .name = intern("bar.o"),
                                          .parent_dir = *src_dir,
                                      });
        REQUIRE(bar_o.has_value());

        auto node = CommandNode {
            .source_dir = intern("src"),
            .instruction_id = intern("echo %2o"),
            .outputs = { *foo_o, *bar_o },
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "echo bar.o");
    }

    SECTION("%% escapes to literal percent")
    {
        auto node = CommandNode {
            .instruction_id = intern("echo 100%%"),
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "echo 100%");
    }

    SECTION("no patterns returns verbatim")
    {
        auto node = CommandNode {
            .instruction_id = intern("cp /src/file /dst/file"),
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "cp /src/file /dst/file");
    }

    SECTION("empty instruction returns empty")
    {
        auto node = CommandNode {};
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(pup::is_empty(result));
    }
}

TEST_CASE("collect_command_dependencies follows order-only deps through groups", "[graph][groups][order-only]")
{
    using pup::LinkType;
    auto bs = make_build_state();
    auto& g = bs.graph;

    // Scenario: c1 produces file1, file1 is added to group1, c2 has order-only dep on group1
    //
    // c1 ---(normal)---> file1 ---(group)---> group1 <---(order-only)--- c2 ---> file2
    //
    // When collecting dependencies of c2, we must find c1 through the group.

    auto c1 = add_command_node(g, CommandNode { .instruction_id = intern("producer") });
    auto file1 = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("file1") });
    auto group1 = add_file_node(g, FileNode { .type = NodeType::Group, .name = intern("<group>") });

    REQUIRE(c1.has_value());
    REQUIRE(file1.has_value());
    REQUIRE(group1.has_value());

    (void)add_edge(g, *c1, *file1, LinkType::Normal);
    (void)add_edge(g, *file1, *group1, LinkType::Group);

    auto c2 = add_command_node(g, CommandNode { .instruction_id = intern("consumer") });
    auto file2 = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("file2") });

    REQUIRE(c2.has_value());
    REQUIRE(file2.has_value());

    (void)add_order_only_edge(g, *group1, *c2);
    (void)add_edge(g, *c2, *file2, LinkType::Normal);

    auto commands = pup::NodeIdMap32 {};
    commands.set(*c2, 1);
    auto deps = pup::cli::collect_command_dependencies(bs, commands);

    REQUIRE(deps.contains(*c2));
    REQUIRE(deps.contains(*c1));
}

TEST_CASE("Topological sort respects order-only deps through groups", "[topo][groups][order-only]")
{
    using pup::LinkType;
    auto bs = make_build_state();
    auto& g = bs.graph;

    // c1: produces file1, file1 is in group1
    auto c1 = add_command_node(g, CommandNode { .instruction_id = intern("producer") });
    auto file1 = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("file1") });
    auto group1 = add_file_node(g, FileNode { .type = NodeType::Group, .name = intern("<group>") });

    REQUIRE(c1.has_value());
    REQUIRE(file1.has_value());
    REQUIRE(group1.has_value());

    (void)add_edge(g, *c1, *file1, LinkType::Normal);
    (void)add_edge(g, *file1, *group1, LinkType::Group);

    // c2: has order-only dep on group1
    auto c2 = add_command_node(g, CommandNode { .instruction_id = intern("consumer") });
    REQUIRE(c2.has_value());
    (void)add_order_only_edge(g, *group1, *c2);

    auto result = topological_sort(g);
    REQUIRE_FALSE(result.has_cycle);

    auto find_pos = [&](pup::NodeId id) -> std::size_t {
        for (auto i = std::size_t { 0 }; i < result.order.size(); ++i) {
            if (result.order[i] == id) {
                return i;
            }
        }
        return result.order.size();
    };

    auto c1_pos = find_pos(*c1);
    auto c2_pos = find_pos(*c2);
    REQUIRE(c1_pos < c2_pos);
}

TEST_CASE("edges_where parameterized edge query", "[graph]")
{
    using pup::LinkType;

    auto bs = make_build_state();
    auto& g = bs.graph;

    auto input = add_file_node(g, FileNode { .name = intern("input.c") });
    auto output = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("output.o") });
    auto tupfile = add_file_node(g, FileNode { .name = intern("Tupfile") });
    auto group = add_file_node(g, FileNode { .type = NodeType::Group, .name = intern("<libs>") });
    auto cmd = add_command_node(g, CommandNode { .instruction_id = intern("gcc") });

    REQUIRE(input.has_value());
    REQUIRE(output.has_value());
    REQUIRE(tupfile.has_value());
    REQUIRE(group.has_value());
    REQUIRE(cmd.has_value());

    (void)add_edge(g, *input, *cmd, LinkType::Normal);
    (void)add_edge(g, *cmd, *output, LinkType::Normal);
    (void)add_edge(g, *tupfile, *cmd, LinkType::Sticky);
    (void)add_order_only_edge(g, *group, *cmd);

    using pup::graph::EdgeDirection;
    using pup::graph::edge_mask::data_flow;
    using pup::graph::edge_mask::inputs;
    using pup::graph::edge_mask::order_only;
    using pup::graph::edge_mask::sticky;

    SECTION("backward data_flow returns Normal inputs only")
    {
        auto result = edges_where(g, *cmd, EdgeDirection::Backward, data_flow);
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == *input);
    }

    SECTION("backward inputs returns Normal + Sticky but not OrderOnly")
    {
        auto result = edges_where(g, *cmd, EdgeDirection::Backward, inputs);
        REQUIRE(result.size() == 2);
        REQUIRE(std::ranges::find(result, *input) != result.end());
        REQUIRE(std::ranges::find(result, *tupfile) != result.end());
        REQUIRE(std::ranges::find(result, *group) == result.end());
    }

    SECTION("backward order_only returns only OO sources")
    {
        auto result = edges_where(g, *cmd, EdgeDirection::Backward, order_only);
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == *group);
    }

    SECTION("forward data_flow from cmd returns output")
    {
        auto result = edges_where(g, *cmd, EdgeDirection::Forward, data_flow);
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == *output);
    }

    SECTION("forward sticky from cmd returns nothing (sticky goes TO cmd)")
    {
        auto result = edges_where(g, *cmd, EdgeDirection::Forward, sticky);
        REQUIRE(result.empty());
    }

    SECTION("backward sticky returns Tupfile")
    {
        auto result = edges_where(g, *cmd, EdgeDirection::Backward, sticky);
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == *tupfile);
    }

    SECTION("combined mask: data_flow | order_only")
    {
        auto combined = static_cast<pup::graph::LinkTypeMask>(data_flow | order_only);
        auto result = edges_where(g, *cmd, EdgeDirection::Backward, combined);
        REQUIRE(result.size() == 2);
        REQUIRE(std::ranges::find(result, *input) != result.end());
        REQUIRE(std::ranges::find(result, *group) != result.end());
    }

    SECTION("equivalence: get_inputs == edges_where backward inputs")
    {
        auto a = get_inputs(g, *cmd);
        auto b = edges_where(g, *cmd, EdgeDirection::Backward, inputs);
        std::ranges::sort(a);
        std::ranges::sort(b);
        REQUIRE(a == b);
    }

    SECTION("equivalence: get_outputs == edges_where forward data_flow")
    {
        auto a = get_outputs(g, *cmd);
        auto b = edges_where(g, *cmd, EdgeDirection::Forward, data_flow);
        std::ranges::sort(a);
        std::ranges::sort(b);
        REQUIRE(a == b);
    }

    SECTION("equivalence: get_order_only == edges_where backward order_only")
    {
        auto a = get_order_only(g, *cmd);
        auto b = edges_where(g, *cmd, EdgeDirection::Backward, order_only);
        std::ranges::sort(a);
        std::ranges::sort(b);
        REQUIRE(a == b);
    }
}

TEST_CASE("collect_affected_commands resolves directory-structured paths", "[graph]")
{
    auto bs = make_build_state();
    auto& g = bs.graph;

    auto src_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("src") });
    auto foo_c = add_file_node(g, FileNode { .name = intern("foo.c"), .parent_dir = *src_dir });
    auto cmd = add_command_node(g, CommandNode { .instruction_id = intern("gcc -c src/foo.c -o foo.o") });
    auto foo_o = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("foo.o") });

    (void)add_edge(g, *foo_c, *cmd);
    (void)add_edge(g, *cmd, *foo_o);

    SECTION("changed source propagates to its command and output")
    {
        auto changed = pup::Vec<pup::StringId> { intern("src/foo.c") };
        auto affected = collect_affected_commands(g, changed);

        REQUIRE(affected.contains(*foo_c));
        REQUIRE(affected.contains(*cmd));
        REQUIRE(affected.contains(*foo_o));
    }

    SECTION("unknown path produces empty result")
    {
        auto changed = pup::Vec<pup::StringId> { intern("src/bar.c") };
        auto affected = collect_affected_commands(g, changed);

        REQUIRE(affected.empty());
    }

    SECTION("generated file marks producing command affected")
    {
        auto cmd2 = add_command_node(g, CommandNode { .instruction_id = intern("link foo.o -o app") });
        auto app = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("app") });
        (void)add_edge(g, *foo_o, *cmd2);
        (void)add_edge(g, *cmd2, *app);

        auto changed = pup::Vec<pup::StringId> { intern("foo.o") };
        auto affected = collect_affected_commands(g, changed);

        REQUIRE(affected.contains(*foo_o));
        REQUIRE(affected.contains(*cmd));
        REQUIRE(affected.contains(*cmd2));
        REQUIRE(affected.contains(*app));
    }
}

TEST_CASE("FileNode path_id populated by add_file_node", "[graph][path_pool]")
{
    auto bs = make_build_state();
    auto& g = bs.graph;

    auto src_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("src") });
    auto foo = add_file_node(g, FileNode { .name = intern("foo.c"), .parent_dir = *src_dir });

    REQUIRE(src_dir.has_value());
    REQUIRE(foo.has_value());

    SECTION("directory node has SourceRoot-grounded path_id")
    {
        auto const* node = get_file_node(g, *src_dir);
        REQUIRE(node != nullptr);
        REQUIRE_FALSE(pup::is_root(node->path_id));
        REQUIRE(g.paths.name(node->path_id) == intern("src"));
        REQUIRE(g.paths.parent(node->path_id) == pup::PathId::SourceRoot);
        REQUIRE(g.paths.root(node->path_id) == pup::PathId::SourceRoot);
    }

    SECTION("file node has chained path_id")
    {
        auto const* node = get_file_node(g, *foo);
        auto const* parent = get_file_node(g, *src_dir);
        REQUIRE(node != nullptr);
        REQUIRE(parent != nullptr);
        REQUIRE(g.paths.name(node->path_id) == intern("foo.c"));
        REQUIRE(g.paths.parent(node->path_id) == parent->path_id);
    }

    SECTION("path_to_node resolves PathId back to NodeId")
    {
        auto const* node = get_file_node(g, *foo);
        REQUIRE(node != nullptr);
        auto const* resolved = g.path_to_node.find(pup::to_underlying(node->path_id));
        REQUIRE(resolved != nullptr);
        REQUIRE(static_cast<pup::NodeId>(*resolved) == *foo);
    }

    SECTION("to_string round-trips through path_id")
    {
        auto const* node = get_file_node(g, *foo);
        REQUIRE(node != nullptr);
        auto& pool = pup::global_pool();
        REQUIRE(sv(g.paths.to_string(node->path_id, pool)) == "src/foo.c");
    }
}
