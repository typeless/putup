// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "graph_support.hpp"
#include "instruction_support.hpp"
#include "pup/cli/config_commands.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/topo.hpp"

#include <algorithm>

using namespace pup::graph;
using pup::LinkType;
using pup::NodeType;

// White-box access for PathPool and interning tests (not part of public API)
namespace pup::graph {
auto get_file_node(Graph const& graph, NodeId id) -> FileNode const*;
auto get_command_node(Graph const& graph, NodeId id) -> CommandNode const*;
} // namespace pup::graph

namespace {
auto sv(pup::StringId id) -> std::string_view { return pup::global_pool().get(id); }
auto intern(std::string_view s) -> pup::StringId { return pup::global_pool().intern(s); }
} // namespace

TEST_CASE("BuildGraph basic operations", "[graph]")
{
    auto bs = make_build_graph();
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
            .parent_dir = dir_id->id(),
        };
        auto file_id = add_file_node(g, file_node);
        REQUIRE(file_id.has_value());

        // Find by (parent_dir, name)
        auto found = find_by_dir_name(g, *dir_id, "foo.c");
        REQUIRE(found.has_value());
        REQUIRE(*found == file_id->id());

        // Not found cases
        REQUIRE_FALSE(find_by_dir_name(g, *dir_id, "bar.c").has_value());
        REQUIRE_FALSE(find_by_dir_name(g, source_root(g), "foo.c").has_value());
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
                                             .parent_dir = src_dir->id(),
                                         });
        auto lib_file = add_file_node(g, FileNode {
                                             .name = intern("util.c"),
                                             .parent_dir = lib_dir->id(),
                                         });
        REQUIRE(src_file.has_value());
        REQUIRE(lib_file.has_value());

        // Each lookup returns the correct node
        auto found_src = find_by_dir_name(g, *src_dir, "util.c");
        auto found_lib = find_by_dir_name(g, *lib_dir, "util.c");
        REQUIRE(found_src.has_value());
        REQUIRE(found_lib.has_value());
        REQUIRE(*found_src == src_file->id());
        REQUIRE(*found_lib == lib_file->id());
        REQUIRE(*found_src != *found_lib);
    }

    SECTION("get_full_path - simple hierarchy")
    {
        // Build: src/foo.c
        auto src_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("src") });
        auto foo_file = add_file_node(g, FileNode {
                                             .name = intern("foo.c"),
                                             .parent_dir = src_dir->id(),
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
        auto b_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("b"), .parent_dir = a_dir->id() });
        auto c_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("c"), .parent_dir = b_dir->id() });
        auto d_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("d"), .parent_dir = c_dir->id() });
        auto file = add_file_node(g, FileNode { .name = intern("file.txt"), .parent_dir = d_dir->id() });

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
        REQUIRE(sv(get_full_path(g, source_root(g))) == "");
        REQUIRE_FALSE(find_file(g, 9999).has_value());
    }

    SECTION("get_full_path - caching works")
    {
        auto dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("cached") });
        auto file = add_file_node(g, FileNode { .name = intern("test.c"), .parent_dir = dir->id() });

        // First call computes and caches
        auto path1 = get_full_path(g, *file, bs.path_cache);
        // Second call should hit cache
        auto path2 = get_full_path(g, *file, bs.path_cache);

        REQUIRE(path1 == "cached/test.c");
        REQUIRE(path2 == "cached/test.c");
    }

    SECTION("clear_path_cache")
    {
        auto dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("dir") });
        auto file = add_file_node(g, FileNode { .name = intern("file.c"), .parent_dir = dir->id() });

        // Populate cache
        (void)get_full_path(g, *file, bs.path_cache);
        (void)get_full_path(g, *dir, bs.path_cache);

        // Clear entire cache
        clear_path_cache(bs.path_cache);
        // Should still work (recomputes)
        REQUIRE(get_full_path(g, *dir, bs.path_cache) == "dir");
        REQUIRE(get_full_path(g, *file, bs.path_cache) == "dir/file.c");
    }

    SECTION("add edges")
    {
        auto id1 = add_file_node(g, FileNode { .name = intern("foo.c") });
        auto id2 = add_command_node(g, CommandNode { .instruction = pup::test::instruction("gcc foo.c") });
        auto id3 = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("foo.o") });

        REQUIRE(id1.has_value());
        REQUIRE(id2.has_value());
        REQUIRE(id3.has_value());

        auto e1 = add_edge(g, id1->id(), id2->id());
        auto e2 = add_edge(g, id2->id(), id3->id());

        REQUIRE(e1.has_value());
        REQUIRE(e2.has_value());

        REQUIRE(edge_count(g) == 2);

        auto inputs = get_inputs(g, id2->id());
        REQUIRE(inputs.size() == 1);
        REQUIRE(inputs[0] == id1->id());

        auto outputs = get_outputs(g, id2->id());
        REQUIRE(outputs.size() == 1);
        REQUIRE(outputs[0] == id3->id());
    }

    SECTION("nodes of type")
    {
        (void)add_file_node(g, FileNode { .name = intern("a.c") });
        (void)add_file_node(g, FileNode { .name = intern("b.c") });
        (void)add_command_node(g, CommandNode { .instruction = pup::test::instruction("gcc") });
        (void)add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("out.o") });

        auto files = pup::test::files_of_type(g, NodeType::File);
        REQUIRE(files.size() == 2);

        auto commands = pup::test::commands_of(g);
        REQUIRE(commands.size() == 1);

        auto generated = pup::test::files_of_type(g, NodeType::Generated);
        REQUIRE(generated.size() == 1);
    }
}

TEST_CASE("Topological sort", "[graph]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;

    SECTION("simple linear graph")
    {
        auto id1 = add_file_node(g, FileNode { .name = intern("a") });
        auto id2 = add_file_node(g, FileNode { .name = intern("b") });
        auto id3 = add_file_node(g, FileNode { .name = intern("c") });

        (void)add_edge(g, id1->id(), id2->id());
        (void)add_edge(g, id2->id(), id3->id());

        auto result = topological_sort(g);
        REQUIRE(result.cycle.empty());
        // 3 nodes we added + BUILD_ROOT_ID = 4
        REQUIRE(result.order.size() == 4);

        // a should come before b, b before c
        auto pos_a = std::ranges::find(result.order, id1->id());
        auto pos_b = std::ranges::find(result.order, id2->id());
        auto pos_c = std::ranges::find(result.order, id3->id());

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

        (void)add_edge(g, id_a->id(), id_b->id());
        (void)add_edge(g, id_a->id(), id_c->id());
        (void)add_edge(g, id_b->id(), id_d->id());
        (void)add_edge(g, id_c->id(), id_d->id());

        auto result = topological_sort(g);
        REQUIRE(result.cycle.empty());
        // 4 nodes we added + BUILD_ROOT_ID = 5
        REQUIRE(result.order.size() == 5);

        // a should come before d, d should come last among our nodes
        auto pos_a = std::ranges::find(result.order, id_a->id());
        auto pos_d = std::ranges::find(result.order, id_d->id());
        REQUIRE(pos_a < pos_d);
    }

    SECTION("cycle detection")
    {
        auto id1 = add_file_node(g, FileNode { .name = intern("a") });
        auto id2 = add_file_node(g, FileNode { .name = intern("b") });
        auto id3 = add_file_node(g, FileNode { .name = intern("c") });

        (void)add_edge(g, id1->id(), id2->id());
        (void)add_edge(g, id2->id(), id3->id());
        (void)add_edge(g, id3->id(), id1->id()); // Creates cycle

        auto result = topological_sort(g);
        REQUIRE_FALSE(result.cycle.empty());
    }

    SECTION("is_dag")
    {
        auto id1 = add_file_node(g, FileNode { .name = intern("a") });
        auto id2 = add_file_node(g, FileNode { .name = intern("b") });

        (void)add_edge(g, id1->id(), id2->id());

        REQUIRE(is_dag(g));

        (void)add_edge(g, id2->id(), id1->id());
        REQUIRE_FALSE(is_dag(g));
    }
}

TEST_CASE("BuildGraph node types", "[graph]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;

    SECTION("Group node type")
    {
        auto node = FileNode { .type = NodeType::Group, .name = intern("{objs}") };
        auto id = add_file_node(g, node);

        REQUIRE(id.has_value());
        REQUIRE(get<NodeType>(g, *id) == NodeType::Group);
    }

    SECTION("get_parent_dir accessor")
    {
        auto dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("src") });
        REQUIRE(dir.has_value());

        auto file = add_file_node(g, FileNode { .name = intern("main.c"), .parent_dir = dir->id() });
        REQUIRE(file.has_value());

        CHECK(get_parent_dir(g, *file) == *dir);
        CHECK(get_parent_dir(g, *dir) == source_root(g));
    }

    SECTION("view<Inputs> and view<Outputs>")
    {
        auto in1 = add_file_node(g, FileNode { .name = intern("a.c") });
        auto in2 = add_file_node(g, FileNode { .name = intern("b.c") });
        auto out = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("a.o") });
        REQUIRE(in1.has_value());
        REQUIRE(in2.has_value());
        REQUIRE(out.has_value());

        auto cmd = add_command_node(g, CommandNode {
            .instruction = pup::test::instruction("gcc -c %f -o %o"),
            .inputs = { in1->id(), in2->id() },
            .outputs = { out->id() },
        });
        REQUIRE(cmd.has_value());

        auto const& inputs = view<Inputs>(g, *cmd);
        REQUIRE(inputs.size() == 2);
        CHECK(inputs[0] == in1->id());
        CHECK(inputs[1] == in2->id());

        auto const& outputs = view<Outputs>(g, *cmd);
        REQUIRE(outputs.size() == 1);
        CHECK(outputs[0] == out->id());
    }

    SECTION("all node types")
    {
        auto file_id = add_file_node(g, FileNode { .name = intern("a.c") });
        auto cmd_id = add_command_node(g, CommandNode { .instruction = pup::test::instruction("gcc") });
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
        REQUIRE(pup::test::files_of_type(g, NodeType::File).size() == 1);
        REQUIRE(pup::test::commands_of(g).size() == 1);
        REQUIRE(pup::test::files_of_type(g, NodeType::Generated).size() == 1);
        REQUIRE(pup::test::files_of_type(g, NodeType::Group).size() == 1);
    }
}

TEST_CASE("get<T> template accessor for unique-return-type properties", "[graph]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;

    auto hash = pup::Hash256 {};
    hash[0] = std::byte { 0xAB };
    hash[31] = std::byte { 0xCD };

    auto dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("src") });
    auto file = add_file_node(g, FileNode {
        .name = intern("main.c"),
        .parent_dir = dir->id(),
        .content_hash = hash,
    });
    auto cmd = add_command_node(g, CommandNode { .instruction = pup::test::instruction("gcc") });
    REQUIRE(dir.has_value());
    REQUIRE(file.has_value());
    REQUIRE(cmd.has_value());

    SECTION("get<NodeType>")
    {
        CHECK(get<NodeType>(g, *file) == NodeType::File);
        CHECK(get<NodeType>(g, *dir) == NodeType::Directory);
        CHECK(pup::node_id::is_command(cmd->id()));
    }

    SECTION("get<Hash256>")
    {
        auto result = get<pup::Hash256>(g, *file);
        CHECK(result[0] == std::byte { 0xAB });
        CHECK(result[31] == std::byte { 0xCD });
    }

    SECTION("get<OutputAction>")
    {
        CHECK(get<OutputAction>(g, *cmd) == OutputAction::Normal);
    }
}

TEST_CASE("BuildGraph edge types", "[graph]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;

    SECTION("order-only edges")
    {
        auto id1 = add_file_node(g, FileNode { .name = intern("header.h") });
        auto id2 = add_command_node(g, CommandNode { .instruction = pup::test::instruction("gcc") });

        REQUIRE(id1.has_value());
        REQUIRE(id2.has_value());

        auto e = add_edge(g, id1->id(), id2->id(), LinkType::OrderOnly);
        REQUIRE(e.has_value());

        auto order_inputs = get_order_only(g, id2->id());
        REQUIRE(order_inputs.size() == 1);
        REQUIRE(order_inputs[0] == id1->id());
    }

    SECTION("group edges")
    {
        // Group edge: command -> group (command produces outputs in group)
        auto cmd_id = add_command_node(g, CommandNode { .instruction = pup::test::instruction("gcc") });
        auto out_id = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("a.o") });
        auto group_id = add_file_node(g, FileNode { .type = NodeType::Group, .name = intern("{objs}") });

        REQUIRE(cmd_id.has_value());
        REQUIRE(out_id.has_value());
        REQUIRE(group_id.has_value());

        (void)add_edge(g, cmd_id->id(), out_id->id());

        auto edge_result = add_edge(g, out_id->id(), group_id->id(), pup::LinkType::Group);
        REQUIRE(edge_result.has_value());
    }
}

TEST_CASE("Graph traversal", "[graph]")
{
    auto bs = make_build_graph();
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

    (void)add_edge(g, id_a->id(), id_b->id());
    (void)add_edge(g, id_a->id(), id_c->id());
    (void)add_edge(g, id_b->id(), id_d->id());
    (void)add_edge(g, id_c->id(), id_e->id());

    SECTION("has_path")
    {
        REQUIRE(has_path(g, id_a->id(), id_d->id()));
        REQUIRE(has_path(g, id_a->id(), id_e->id()));
        REQUIRE_FALSE(has_path(g, id_d->id(), id_a->id()));
        REQUIRE_FALSE(has_path(g, id_b->id(), id_c->id()));
    }
}

TEST_CASE("Order-only dependencies in topological sort", "[graph]")
{
    auto bs = make_build_graph();
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
    auto cmd_id = add_command_node(g, CommandNode { .instruction = pup::test::instruction("gcc") });
    auto output_id = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("output.o") });

    REQUIRE(header_id.has_value());
    REQUIRE(input_id.has_value());
    REQUIRE(cmd_id.has_value());
    REQUIRE(output_id.has_value());

    // Normal edge: input.c -> cmd
    (void)add_edge(g, input_id->id(), cmd_id->id());
    // Normal edge: cmd -> output.o
    (void)add_edge(g, cmd_id->id(), output_id->id());
    // Order-only edge: header.h -> cmd
    (void)add_edge(g, header_id->id(), cmd_id->id(), LinkType::OrderOnly);

    SECTION("get_order_only_dependents returns correct nodes")
    {
        auto dependents = get_order_only_dependents(g, header_id->id());
        REQUIRE(dependents.size() == 1);
        REQUIRE(dependents[0] == cmd_id->id());
    }

    SECTION("topological sort respects order-only dependencies")
    {
        auto result = topological_sort(g);
        REQUIRE(result.cycle.empty());

        // Find positions in the sorted order
        auto find_pos = [&](pup::NodeId id) -> std::size_t {
            for (std::size_t i = 0; i < result.order.size(); ++i) {
                if (result.order[i] == id) {
                    return i;
                }
            }
            return result.order.size();
        };

        auto header_pos = find_pos(header_id->id());
        auto input_pos = find_pos(input_id->id());
        auto cmd_pos = find_pos(cmd_id->id());
        auto output_pos = find_pos(output_id->id());

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
        (void)add_edge(g, output_id->id(), header_id->id(), LinkType::OrderOnly);

        auto result = topological_sort(g);
        REQUIRE_FALSE(result.cycle.empty());
    }
}

TEST_CASE("Unified edge storage for order-only edges", "[graph]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;

    // group1 ---(order-only)---> cmd ---(normal)---> output.o
    //                             ^
    // input.c ----(normal)-------+

    auto group_id = add_file_node(g, FileNode { .type = NodeType::Group, .name = intern("<libs>") });
    auto input_id = add_file_node(g, FileNode { .name = intern("input.c") });
    auto cmd_id = add_command_node(g, CommandNode { .instruction = pup::test::instruction("gcc") });
    auto output_id = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("output.o") });

    REQUIRE(group_id.has_value());
    REQUIRE(input_id.has_value());
    REQUIRE(cmd_id.has_value());
    REQUIRE(output_id.has_value());

    (void)add_edge(g, input_id->id(), cmd_id->id());
    (void)add_edge(g, cmd_id->id(), output_id->id());
    (void)add_edge(g, group_id->id(), cmd_id->id(), LinkType::OrderOnly);

    SECTION("order-only edges are counted in edge_count")
    {
        REQUIRE(edge_count(g) == 3);
    }

    SECTION("get_inputs excludes order-only sources")
    {
        auto inputs = get_inputs(g, cmd_id->id());
        REQUIRE(inputs.size() == 1);
        REQUIRE(inputs[0] == input_id->id());
    }

    SECTION("get_outputs excludes order-only targets")
    {
        auto outputs = get_outputs(g, group_id->id());
        REQUIRE(outputs.empty());
    }

    SECTION("get_order_only returns correct sources via unified index")
    {
        auto oo = get_order_only(g, cmd_id->id());
        REQUIRE(oo.size() == 1);
        REQUIRE(oo[0] == group_id->id());
    }

    SECTION("get_order_only_dependents returns correct targets via unified index")
    {
        auto deps = get_order_only_dependents(g, group_id->id());
        REQUIRE(deps.size() == 1);
        REQUIRE(deps[0] == cmd_id->id());
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
    auto bs = make_build_graph();
    auto& g = bs.graph;

    // Scenario: Tupfile defines a command, source.c is input
    //
    // Tupfile ---(sticky)---> cmd ---(normal)---> output.o
    //                          ^
    // source.c ---(normal)-----+

    auto tupfile_id = add_file_node(g, FileNode { .name = intern("Tupfile") });
    auto source_id = add_file_node(g, FileNode { .name = intern("source.c") });
    auto cmd_id = add_command_node(g, CommandNode { .instruction = pup::test::instruction("gcc source.c") });
    auto output_id = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("output.o") });

    (void)add_edge(g, source_id->id(), cmd_id->id(), LinkType::Normal);
    (void)add_edge(g, cmd_id->id(), output_id->id(), LinkType::Normal);
    (void)add_edge(g, tupfile_id->id(), cmd_id->id(), LinkType::Sticky);

    // THE KEY ASSERTION: Tupfile's get_outputs() must NOT include the command
    // Old behavior: tupfile_outputs contained cmd_id (WRONG - causes cascade)
    // New behavior: tupfile_outputs is empty (CORRECT - sticky edges excluded)
    auto tupfile_outputs = get_outputs(g, tupfile_id->id());
    REQUIRE(tupfile_outputs.empty());

    // Normal edges still work as expected
    auto source_outputs = get_outputs(g, source_id->id());
    REQUIRE(source_outputs.size() == 1);
    REQUIRE(source_outputs[0] == cmd_id->id());

    auto cmd_outputs = get_outputs(g, cmd_id->id());
    REQUIRE(cmd_outputs.size() == 1);
    REQUIRE(cmd_outputs[0] == output_id->id());
}

TEST_CASE("Sticky edge API", "[graph]")
{
    using pup::LinkType;
    auto bs = make_build_graph();
    auto& g = bs.graph;

    auto tupfile_id = add_file_node(g, FileNode { .name = intern("Tupfile") });
    auto source_id = add_file_node(g, FileNode { .name = intern("source.c") });
    auto cmd_id = add_command_node(g, CommandNode { .instruction = pup::test::instruction("gcc source.c") });
    auto output_id = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("output.o") });

    (void)add_edge(g, source_id->id(), cmd_id->id(), LinkType::Normal);
    (void)add_edge(g, cmd_id->id(), output_id->id(), LinkType::Normal);
    (void)add_edge(g, tupfile_id->id(), cmd_id->id(), LinkType::Sticky);

    SECTION("edges_where sticky returns only sticky edges")
    {
        auto tupfile_sticky = edges_where(g, tupfile_id->id(), EdgeDirection::Forward, edge_mask::sticky);
        REQUIRE(tupfile_sticky.size() == 1);
        REQUIRE(tupfile_sticky[0] == cmd_id->id());

        REQUIRE(edges_where(g, source_id->id(), EdgeDirection::Forward, edge_mask::sticky).empty());
        REQUIRE(edges_where(g, cmd_id->id(), EdgeDirection::Forward, edge_mask::sticky).empty());
    }

    SECTION("inputs include all edge types")
    {
        auto cmd_inputs = get_inputs(g, cmd_id->id());
        REQUIRE(cmd_inputs.size() == 2);
        REQUIRE(std::ranges::find(cmd_inputs, tupfile_id->id()) != cmd_inputs.end());
        REQUIRE(std::ranges::find(cmd_inputs, source_id->id()) != cmd_inputs.end());
    }

    SECTION("edges() returns all edges including sticky")
    {
        REQUIRE(edge_count(g) == 3);

        auto edges = g.edges;
        auto sticky_edge = std::ranges::find_if(edges, [](auto const& e) {
            return e.type == LinkType::Sticky;
        });
        REQUIRE(sticky_edge != edges.end());
        REQUIRE(sticky_edge->from == tupfile_id->id());
        REQUIRE(sticky_edge->to == cmd_id->id());
    }

    SECTION("multiple sticky edges from same node")
    {
        auto cmd2_id = add_command_node(g, CommandNode { .instruction = pup::test::instruction("gcc other.c") });
        (void)add_edge(g, tupfile_id->id(), cmd2_id->id(), LinkType::Sticky);

        REQUIRE(edges_where(g, tupfile_id->id(), EdgeDirection::Forward, edge_mask::sticky).size() == 2);
        REQUIRE(get_outputs(g, tupfile_id->id()).empty());
    }

    SECTION("mixed edge types from same node")
    {
        auto cmd2_id = add_command_node(g, CommandNode { .instruction = pup::test::instruction("lint source.c") });
        (void)add_edge(g, source_id->id(), cmd2_id->id(), LinkType::Sticky);

        auto source_outputs = get_outputs(g, source_id->id());
        REQUIRE(source_outputs.size() == 1);
        REQUIRE(source_outputs[0] == cmd_id->id());

        auto source_sticky = edges_where(g, source_id->id(), EdgeDirection::Forward, edge_mask::sticky);
        REQUIRE(source_sticky.size() == 1);
        REQUIRE(source_sticky[0] == cmd2_id->id());
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

TEST_CASE("CommandNode stores an instruction", "[graph][instruction]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;

    SECTION("command node with an instruction")
    {
        auto node = CommandNode {
            .display = intern("CC foo.o"),
            .instruction = pup::test::instruction("gcc -O2 -c -o %o %f"),
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto const* cmd = get_command_node(g, cmd_id->id());
        REQUIRE(cmd != nullptr);
        REQUIRE(cmd->instruction.empty() == false);
        REQUIRE(sv(get<InstructionPattern>(g, *cmd_id)) == "gcc -O2 -c -o %o %f");
    }

    SECTION("command node with literal instruction (no patterns)")
    {
        auto node = CommandNode {
            .instruction = pup::test::instruction("cp foo bar"),
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto const* cmd = get_command_node(g, cmd_id->id());
        REQUIRE(cmd != nullptr);
        REQUIRE(cmd->instruction.empty() == false);
        REQUIRE(sv(get<InstructionPattern>(g, *cmd_id)) == "cp foo bar");
        REQUIRE(sv(expand_instruction(g, *cmd_id)) == "cp foo bar");
    }

    SECTION("command node with no instruction")
    {
        auto node = CommandNode {};
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto const* cmd = get_command_node(g, cmd_id->id());
        REQUIRE(cmd != nullptr);
        REQUIRE(cmd->instruction.empty());
        REQUIRE(sv(get<InstructionPattern>(g, *cmd_id)).empty());
    }

    SECTION("multiple commands share same instruction")
    {
        auto node1 = CommandNode {
            .instruction = pup::test::instruction("gcc -c -o %o %f"),
        };
        auto node2 = CommandNode {
            .instruction = pup::test::instruction("gcc -c -o %o %f"),
        };

        auto cmd1_id = add_command_node(g, std::move(node1));
        auto cmd2_id = add_command_node(g, std::move(node2));

        REQUIRE(cmd1_id.has_value());
        REQUIRE(cmd2_id.has_value());

        auto const* cmd1 = get_command_node(g, cmd1_id->id());
        auto const* cmd2 = get_command_node(g, cmd2_id->id());

        REQUIRE(render_instruction(cmd1->instruction) == render_instruction(cmd2->instruction));
        REQUIRE(sv(get<InstructionPattern>(g, *cmd1_id)) == sv(get<InstructionPattern>(g, *cmd2_id)));
    }
}

TEST_CASE("expand_instruction reconstructs command from operands", "[graph][instruction]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;

    // Build directory hierarchy: src/foo.c, src/bar.c, src/foo.o
    auto src_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("src") });
    REQUIRE(src_dir.has_value());

    auto foo_c = add_file_node(g, FileNode {
                                      .name = intern("foo.c"),
                                      .parent_dir = src_dir->id(),
                                  });
    auto bar_c = add_file_node(g, FileNode {
                                      .name = intern("bar.c"),
                                      .parent_dir = src_dir->id(),
                                  });
    auto foo_o = add_file_node(g, FileNode {
                                      .type = NodeType::Generated,
                                      .name = intern("foo.o"),
                                      .parent_dir = src_dir->id(),
                                  });
    REQUIRE(foo_c.has_value());
    REQUIRE(bar_c.has_value());
    REQUIRE(foo_o.has_value());

    SECTION("%f and %o with source_dir")
    {
        auto node = CommandNode {
            .source_dir = intern("src"),
            .instruction = pup::test::instruction("gcc -c %f -o %o"),
            .inputs = { foo_c->id() },
            .outputs = { foo_o->id() },
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
            .instruction = pup::test::instruction("gcc -c %f -o %o"),
            .inputs = { foo_c->id(), bar_c->id() },
            .outputs = { foo_o->id() },
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "gcc -c foo.c bar.c -o foo.o");
    }

    SECTION("%o with multiple outputs")
    {
        auto bar_o = add_file_node(g, FileNode {
                                          .type = NodeType::Generated,
                                          .name = intern("bar.o"),
                                          .parent_dir = src_dir->id(),
                                      });
        REQUIRE(bar_o.has_value());

        auto node = CommandNode {
            .source_dir = intern("src"),
            .instruction = pup::test::instruction("gen %f -o %o"),
            .inputs = { foo_c->id() },
            .outputs = { foo_o->id(), bar_o->id() },
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "gen foo.c -o foo.o bar.o");
    }

    SECTION("%b (basename of the one input)")
    {
        auto node = CommandNode {
            .source_dir = intern("src"),
            .instruction = pup::test::instruction("echo %b"),
            .inputs = { foo_c->id() },
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "echo foo.c");
    }

    SECTION("%b and %B name every input")
    {
        auto node = CommandNode {
            .source_dir = intern("src"),
            .instruction = pup::test::instruction("echo %b %B"),
            .inputs = { foo_c->id(), bar_c->id() },
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "echo foo.c bar.c foo bar");
    }

    SECTION("%B (stem of the one input)")
    {
        auto node = CommandNode {
            .source_dir = intern("src"),
            .instruction = pup::test::instruction("echo %B"),
            .inputs = { foo_c->id() },
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
            .instruction = pup::test::instruction("echo %e"),
            .inputs = { foo_c->id() },
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
            .instruction = pup::test::instruction("echo %d"),
            .inputs = { foo_c->id() },
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
            .instruction = pup::test::instruction("echo %d"),
            .inputs = { foo_c->id() },
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "echo baz");
    }

    SECTION("%O (the one output without its extension)")
    {
        auto node = CommandNode {
            .source_dir = intern("src"),
            .instruction = pup::test::instruction("echo %O"),
            .outputs = { foo_o->id() },
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "echo foo");
    }

    SECTION("%Nf (N-th input)")
    {
        auto node = CommandNode {
            .source_dir = intern("src"),
            .instruction = pup::test::instruction("gcc %1f %2f"),
            .inputs = { foo_c->id(), bar_c->id() },
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
                                          .parent_dir = src_dir->id(),
                                      });
        REQUIRE(bar_o.has_value());

        auto node = CommandNode {
            .source_dir = intern("src"),
            .instruction = pup::test::instruction("echo %2o"),
            .outputs = { foo_o->id(), bar_o->id() },
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "echo bar.o");
    }

    SECTION("%% escapes to literal percent")
    {
        auto node = CommandNode {
            .instruction = pup::test::instruction("echo 100%%"),
        };
        auto cmd_id = add_command_node(g, std::move(node));
        REQUIRE(cmd_id.has_value());

        auto result = expand_instruction(g, *cmd_id);
        CHECK(sv(result) == "echo 100%");
    }

    SECTION("no patterns returns verbatim")
    {
        auto node = CommandNode {
            .instruction = pup::test::instruction("cp /src/file /dst/file"),
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
    auto bs = make_build_graph();
    auto& g = bs.graph;

    // Scenario: c1 produces file1, file1 is added to group1, c2 has order-only dep on group1
    //
    // c1 ---(normal)---> file1 ---(group)---> group1 <---(order-only)--- c2 ---> file2
    //
    // When collecting dependencies of c2, we must find c1 through the group.

    auto c1 = add_command_node(g, CommandNode { .instruction = pup::test::instruction("producer") });
    auto file1 = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("file1") });
    auto group1 = add_file_node(g, FileNode { .type = NodeType::Group, .name = intern("<group>") });

    REQUIRE(c1.has_value());
    REQUIRE(file1.has_value());
    REQUIRE(group1.has_value());

    (void)add_edge(g, c1->id(), file1->id(), LinkType::Normal);
    (void)add_edge(g, file1->id(), group1->id(), LinkType::Group);

    auto c2 = add_command_node(g, CommandNode { .instruction = pup::test::instruction("consumer") });
    auto file2 = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("file2") });

    REQUIRE(c2.has_value());
    REQUIRE(file2.has_value());

    (void)add_edge(g, group1->id(), c2->id(), LinkType::OrderOnly);
    (void)add_edge(g, c2->id(), file2->id(), LinkType::Normal);

    auto commands = pup::NodeIdMap32 {};
    commands.set(c2->id(), 1);
    auto deps = pup::cli::collect_command_dependencies(bs, commands);

    REQUIRE(deps.contains(c2->id()));
    REQUIRE(deps.contains(c1->id()));
}

TEST_CASE("Topological sort respects order-only deps through groups", "[topo][groups][order-only]")
{
    using pup::LinkType;
    auto bs = make_build_graph();
    auto& g = bs.graph;

    // c1: produces file1, file1 is in group1
    auto c1 = add_command_node(g, CommandNode { .instruction = pup::test::instruction("producer") });
    auto file1 = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("file1") });
    auto group1 = add_file_node(g, FileNode { .type = NodeType::Group, .name = intern("<group>") });

    REQUIRE(c1.has_value());
    REQUIRE(file1.has_value());
    REQUIRE(group1.has_value());

    (void)add_edge(g, c1->id(), file1->id(), LinkType::Normal);
    (void)add_edge(g, file1->id(), group1->id(), LinkType::Group);

    // c2: has order-only dep on group1
    auto c2 = add_command_node(g, CommandNode { .instruction = pup::test::instruction("consumer") });
    REQUIRE(c2.has_value());
    (void)add_edge(g, group1->id(), c2->id(), LinkType::OrderOnly);

    auto result = topological_sort(g);
    REQUIRE(result.cycle.empty());

    auto find_pos = [&](pup::NodeId id) -> std::size_t {
        for (auto i = std::size_t { 0 }; i < result.order.size(); ++i) {
            if (result.order[i] == id) {
                return i;
            }
        }
        return result.order.size();
    };

    auto c1_pos = find_pos(c1->id());
    auto c2_pos = find_pos(c2->id());
    REQUIRE(c1_pos < c2_pos);
}

TEST_CASE("Every edge mask follows from one role per link type", "[graph]")
{
    using pup::LinkRole;
    using pup::LinkType;
    using pup::graph::link_type_bit;

    SECTION("each link type carries the role its routers assume")
    {
        REQUIRE(pup::link_role(LinkType::Normal) == LinkRole::DataFlow);
        REQUIRE(pup::link_role(LinkType::Group) == LinkRole::DataFlow);
        REQUIRE(pup::link_role(LinkType::Implicit) == LinkRole::DataFlow);
        REQUIRE(pup::link_role(LinkType::OrderOnly) == LinkRole::Ordering);
        REQUIRE(pup::link_role(LinkType::Sticky) == LinkRole::Identity);
    }

    SECTION("each mask holds exactly the types its name claims")
    {
        REQUIRE(
            pup::graph::edge_mask::data_flow
            == (link_type_bit(LinkType::Normal) | link_type_bit(LinkType::Group) | link_type_bit(LinkType::Implicit))
        );
        REQUIRE(pup::graph::edge_mask::order_only == link_type_bit(LinkType::OrderOnly));
        REQUIRE(pup::graph::edge_mask::sticky == link_type_bit(LinkType::Sticky));
        REQUIRE(
            pup::graph::edge_mask::inputs
            == (link_type_bit(LinkType::Normal) | link_type_bit(LinkType::Sticky) | link_type_bit(LinkType::Group)
                | link_type_bit(LinkType::Implicit))
        );
        REQUIRE(
            pup::graph::edge_mask::consumers
            == (link_type_bit(LinkType::Normal) | link_type_bit(LinkType::Group) | link_type_bit(LinkType::Implicit)
                | link_type_bit(LinkType::OrderOnly))
        );
    }

    SECTION("the two index-side walks partition every consumption edge")
    {
        using pup::graph::edge_mask::consumers;
        using pup::graph::edge_mask::discovered_consumers;
        using pup::graph::edge_mask::parsed_consumers;

        REQUIRE((parsed_consumers | discovered_consumers) == consumers);
        REQUIRE((parsed_consumers & discovered_consumers) == 0);
        REQUIRE(discovered_consumers == link_type_bit(LinkType::Implicit));
        REQUIRE(
            parsed_consumers
            == (link_type_bit(LinkType::Normal) | link_type_bit(LinkType::Group) | link_type_bit(LinkType::OrderOnly))
        );
    }

    SECTION("a link type this build does not know routes nothing")
    {
        // Index edge types are deserialized without validation, so membership has to answer for
        // every byte: 33 shifts past the mask width and read as Normal until in_mask was total.
        for (auto byte : { 0, 6, 8, 9, 33, 255 }) {
            auto const unknown = static_cast<LinkType>(byte);
            REQUIRE(pup::link_role(unknown) == LinkRole::Unknown);
            REQUIRE_FALSE(pup::graph::in_mask(unknown, pup::graph::edge_mask::consumers));
            REQUIRE_FALSE(pup::graph::in_mask(unknown, pup::graph::edge_mask::parsed_consumers));
            REQUIRE_FALSE(pup::graph::in_mask(unknown, pup::graph::edge_mask::discovered_consumers));
            REQUIRE_FALSE(pup::graph::in_mask(unknown, pup::graph::edge_mask::inputs));
        }
    }

    SECTION("a new link type must state whether the scope-crossing bypass carries it")
    {
        // The switch has no default, so a new LinkType stops compiling here until someone
        // decides whether a scoped build must notice its content changing. Deriving both sides
        // from link_role instead would move together and answer nothing — which is how a
        // declared source input stayed invisible to every scoped build (#200, #189).
        auto const carried_by_bypass = [](LinkType t) {
            switch (t) {
            case LinkType::Normal:
            case LinkType::Group:
            case LinkType::Implicit:
            case LinkType::Sticky:
                return true;
            case LinkType::OrderOnly:
                return false;
            }
            return false;
        };

        // Sweeping the mask's width rather than listing types: the switch above demands the
        // statement, this checks it, and neither needs editing when a sixth type arrives.
        for (auto value = std::uint8_t { 1 }; value <= std::numeric_limits<pup::graph::LinkTypeMask>::digits; ++value) {
            auto const type = static_cast<LinkType>(value);
            if (pup::link_role(type) == LinkRole::Unknown) {
                continue;
            }
            REQUIRE(pup::graph::in_mask(type, pup::graph::edge_mask::inputs) == carried_by_bypass(type));
        }
    }

    SECTION("every known link type is in the masks its role implies")
    {
        for (auto type : { LinkType::Normal, LinkType::Sticky, LinkType::Group, LinkType::Implicit, LinkType::OrderOnly }) {
            REQUIRE(pup::graph::in_mask(type, pup::graph::edge_mask::consumers) == (pup::link_role(type) != LinkRole::Identity));
            REQUIRE(pup::graph::in_mask(type, pup::graph::edge_mask::sticky) == (pup::link_role(type) == LinkRole::Identity));
        }
    }
}

TEST_CASE("edges_where parameterized edge query", "[graph]")
{
    using pup::LinkType;

    auto bs = make_build_graph();
    auto& g = bs.graph;

    auto input = add_file_node(g, FileNode { .name = intern("input.c") });
    auto output = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("output.o") });
    auto tupfile = add_file_node(g, FileNode { .name = intern("Tupfile") });
    auto group = add_file_node(g, FileNode { .type = NodeType::Group, .name = intern("<libs>") });
    auto cmd = add_command_node(g, CommandNode { .instruction = pup::test::instruction("gcc") });

    REQUIRE(input.has_value());
    REQUIRE(output.has_value());
    REQUIRE(tupfile.has_value());
    REQUIRE(group.has_value());
    REQUIRE(cmd.has_value());

    (void)add_edge(g, input->id(), cmd->id(), LinkType::Normal);
    (void)add_edge(g, cmd->id(), output->id(), LinkType::Normal);
    (void)add_edge(g, tupfile->id(), cmd->id(), LinkType::Sticky);
    (void)add_edge(g, group->id(), cmd->id(), LinkType::OrderOnly);

    using pup::graph::EdgeDirection;
    using pup::graph::edge_mask::data_flow;
    using pup::graph::edge_mask::inputs;
    using pup::graph::edge_mask::order_only;
    using pup::graph::edge_mask::sticky;

    SECTION("backward data_flow returns Normal inputs only")
    {
        auto result = edges_where(g, cmd->id(), EdgeDirection::Backward, data_flow);
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == input->id());
    }

    SECTION("backward inputs returns Normal + Sticky but not OrderOnly")
    {
        auto result = edges_where(g, cmd->id(), EdgeDirection::Backward, inputs);
        REQUIRE(result.size() == 2);
        REQUIRE(std::ranges::find(result, input->id()) != result.end());
        REQUIRE(std::ranges::find(result, tupfile->id()) != result.end());
        REQUIRE(std::ranges::find(result, group->id()) == result.end());
    }

    SECTION("backward order_only returns only OO sources")
    {
        auto result = edges_where(g, cmd->id(), EdgeDirection::Backward, order_only);
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == group->id());
    }

    SECTION("forward data_flow from cmd returns output")
    {
        auto result = edges_where(g, cmd->id(), EdgeDirection::Forward, data_flow);
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == output->id());
    }

    SECTION("forward sticky from cmd returns nothing (sticky goes TO cmd)")
    {
        auto result = edges_where(g, cmd->id(), EdgeDirection::Forward, sticky);
        REQUIRE(result.empty());
    }

    SECTION("backward sticky returns Tupfile")
    {
        auto result = edges_where(g, cmd->id(), EdgeDirection::Backward, sticky);
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == tupfile->id());
    }

    SECTION("combined mask: data_flow | order_only")
    {
        auto combined = static_cast<pup::graph::LinkTypeMask>(data_flow | order_only);
        auto result = edges_where(g, cmd->id(), EdgeDirection::Backward, combined);
        REQUIRE(result.size() == 2);
        REQUIRE(std::ranges::find(result, input->id()) != result.end());
        REQUIRE(std::ranges::find(result, group->id()) != result.end());
    }

    SECTION("equivalence: get_inputs == edges_where backward inputs")
    {
        auto a = get_inputs(g, cmd->id());
        auto b = edges_where(g, cmd->id(), EdgeDirection::Backward, inputs);
        std::ranges::sort(a);
        std::ranges::sort(b);
        REQUIRE(a == b);
    }

    SECTION("equivalence: get_outputs == edges_where forward data_flow")
    {
        auto a = get_outputs(g, cmd->id());
        auto b = edges_where(g, cmd->id(), EdgeDirection::Forward, data_flow);
        std::ranges::sort(a);
        std::ranges::sort(b);
        REQUIRE(a == b);
    }

    SECTION("equivalence: get_order_only == edges_where backward order_only")
    {
        auto a = get_order_only(g, cmd->id());
        auto b = edges_where(g, cmd->id(), EdgeDirection::Backward, order_only);
        std::ranges::sort(a);
        std::ranges::sort(b);
        REQUIRE(a == b);
    }
}

TEST_CASE("collect_affected_commands resolves directory-structured paths", "[graph]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;

    auto src_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("src") });
    auto foo_c = add_file_node(g, FileNode { .name = intern("foo.c"), .parent_dir = src_dir->id() });
    auto cmd = add_command_node(g, CommandNode { .instruction = pup::test::instruction("gcc -c src/foo.c -o foo.o") });
    auto foo_o = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("foo.o") });

    (void)add_edge(g, foo_c->id(), cmd->id());
    (void)add_edge(g, cmd->id(), foo_o->id());

    SECTION("changed source propagates to its command and output")
    {
        auto changed = pup::Vec<pup::StringId> { intern("src/foo.c") };
        auto affected = collect_affected_commands(g, changed);

        REQUIRE(affected.contains(foo_c->id()));
        REQUIRE(affected.contains(cmd->id()));
        REQUIRE(affected.contains(foo_o->id()));
    }

    SECTION("unknown path produces empty result")
    {
        auto changed = pup::Vec<pup::StringId> { intern("src/bar.c") };
        auto affected = collect_affected_commands(g, changed);

        REQUIRE(affected.empty());
    }

    SECTION("generated file marks producing command affected")
    {
        auto cmd2 = add_command_node(g, CommandNode { .instruction = pup::test::instruction("link foo.o -o app") });
        auto app = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("app") });
        (void)add_edge(g, foo_o->id(), cmd2->id());
        (void)add_edge(g, cmd2->id(), app->id());

        auto changed = pup::Vec<pup::StringId> { intern("foo.o") };
        auto affected = collect_affected_commands(g, changed);

        REQUIRE(affected.contains(foo_o->id()));
        REQUIRE(affected.contains(cmd->id()));
        REQUIRE(affected.contains(cmd2->id()));
        REQUIRE(affected.contains(app->id()));
    }
}

TEST_CASE("A forced command brings the scanner that reports what it read", "[graph][implicit]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;

    auto foo_c = add_file_node(g, FileNode { .name = intern("foo.c") });
    auto gate = add_command_node(g, CommandNode { .instruction = pup::test::instruction("gcc -c foo.c -o /dev/null") });
    REQUIRE(foo_c.has_value());
    REQUIRE(gate.has_value());

    auto scanner = add_command_node(g, CommandNode { .instruction = pup::test::instruction("gcc -MM foo.c"), .parent_command = gate->id() });
    REQUIRE(scanner.has_value());

    (void)add_edge(g, foo_c->id(), scanner->id());
    (void)add_edge(g, scanner->id(), gate->id());

    SECTION("a command forced with no changed file still pulls its scanner")
    {
        // The channel that carries a forced command differs per caller, but the scanner must
        // follow all of them: running the parent alone retires its recorded deps (#228).
        auto affected = collect_affected_commands(g, {}, pup::Vec<pup::NodeId> { gate->id() });

        REQUIRE(affected.contains(gate->id()));
        REQUIRE(affected.contains(scanner->id()));
    }

    SECTION("forcing does not make the forced command's consumers run")
    {
        auto out = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("out.o") });
        auto consumer = add_command_node(g, CommandNode { .instruction = pup::test::instruction("link out.o") });
        REQUIRE(out.has_value());
        REQUIRE(consumer.has_value());
        (void)add_edge(g, gate->id(), out->id());
        (void)add_edge(g, out->id(), consumer->id());

        auto affected = collect_affected_commands(g, {}, pup::Vec<pup::NodeId> { gate->id() });

        REQUIRE(affected.contains(gate->id()));
        REQUIRE_FALSE(affected.contains(consumer->id()));
    }
}

TEST_CASE("A recorded discovery routes the consumer no edge points at", "[graph][implicit]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;

    auto seed = add_file_node(g, FileNode { .name = intern("seed.txt") });
    auto producer = add_command_node(g, CommandNode { .instruction = pup::test::instruction("cat seed.txt > p.txt") });
    auto p_txt = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("p.txt") });
    auto consumer = add_command_node(g, CommandNode { .instruction = pup::test::instruction("sh gen.sh c.o") });
    auto c_o = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("c.o") });
    REQUIRE(seed.has_value());
    REQUIRE(producer.has_value());
    REQUIRE(p_txt.has_value());
    REQUIRE(consumer.has_value());
    REQUIRE(c_o.has_value());

    (void)add_edge(g, seed->id(), producer->id());
    (void)add_edge(g, producer->id(), p_txt->id());
    (void)add_edge(g, consumer->id(), c_o->id());

    auto const discovery = pup::Vec<pup::OrderingEdge> {
        pup::OrderingEdge { .producer = producer->id(), .consumer = consumer->id() }
    };

    SECTION("a change upstream of the producer carries on past the consumer")
    {
        auto changed = pup::Vec<pup::StringId> { intern("seed.txt") };
        auto affected = collect_affected_commands(g, changed, {}, discovery);

        REQUIRE(affected.contains(producer->id()));
        REQUIRE(affected.contains(consumer->id()));
        REQUIRE(affected.contains(c_o->id()));
    }

    SECTION("forcing the producer does not recruit the consumer")
    {
        // RED if the expansion moves below the forced join.
        auto affected = collect_affected_commands(g, {}, pup::Vec<pup::NodeId> { producer->id() }, discovery);

        REQUIRE(affected.contains(producer->id()));
        REQUIRE_FALSE(affected.contains(consumer->id()));
    }

    SECTION("a routed consumer brings the scanner that reports what it read")
    {
        // RED if the expansion moves below the scanner sweep: #228 by another door.
        auto scanner = add_command_node(g, CommandNode { .instruction = pup::test::instruction("gcc -MM c.c"), .parent_command = consumer->id() });
        REQUIRE(scanner.has_value());

        auto changed = pup::Vec<pup::StringId> { intern("seed.txt") };
        auto affected = collect_affected_commands(g, changed, {}, discovery);

        REQUIRE(affected.contains(consumer->id()));
        REQUIRE(affected.contains(scanner->id()));
    }
}

TEST_CASE("add_file_node refuses a second node at an occupied path", "[graph][path_pool]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;

    auto src_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("src") });
    REQUIRE(src_dir.has_value());
    auto first = add_file_node(g, FileNode { .name = intern("foo.c"), .parent_dir = src_dir->id() });
    REQUIRE(first.has_value());

    SECTION("a second addressable node with the same parent and name is an error")
    {
        auto second = add_file_node(g, FileNode { .name = intern("foo.c"), .parent_dir = src_dir->id() });
        REQUIRE_FALSE(second.has_value());
        REQUIRE(second.error().code == pup::ErrorCode::DuplicateNode);
    }

    SECTION("the node already at the path stays reachable by both lookups")
    {
        auto const node_count = g.files.size();
        auto second = add_file_node(g, FileNode { .name = intern("foo.c"), .parent_dir = src_dir->id() });
        REQUIRE_FALSE(second.has_value());

        auto const* first_node = get_file_node(g, first->id());
        REQUIRE(first_node != nullptr);
        auto const* resolved = g.path_to_node.find(pup::to_underlying(first_node->path_id));
        REQUIRE(resolved != nullptr);
        REQUIRE(static_cast<pup::NodeId>(*resolved) == first->id());
        REQUIRE(find_by_dir_name(g, *src_dir, "foo.c") == first->id());
        REQUIRE(g.files.size() == node_count);
    }

    SECTION("a node of a different type at the same path is refused just the same")
    {
        auto second = add_file_node(g, FileNode {
            .type = NodeType::Generated,
            .name = intern("foo.c"),
            .parent_dir = src_dir->id(),
        });
        REQUIRE_FALSE(second.has_value());
        REQUIRE(second.error().code == pup::ErrorCode::DuplicateNode);
    }

    SECTION("the build root's name is occupied even though no path resolves to it")
    {
        REQUIRE(set_build_root_name(bs, "out").has_value());
        auto clash = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("out") });
        REQUIRE_FALSE(clash.has_value());
        REQUIRE(clash.error().code == pup::ErrorCode::DuplicateNode);
        REQUIRE(pup::global_pool().get(clash.error().message).find("directory") != std::string_view::npos);
    }

    SECTION("the message names the kind of node already at the path")
    {
        auto second = add_file_node(g, FileNode { .name = intern("foo.c"), .parent_dir = src_dir->id() });
        REQUIRE_FALSE(second.has_value());
        auto const message = pup::global_pool().get(second.error().message);
        REQUIRE(message.find("src/foo.c") != std::string_view::npos);
        REQUIRE(message.find("a file already occupies") != std::string_view::npos);
    }

    SECTION("a node that names no path may share a name with one that does")
    {
        auto var = add_file_node(g, FileNode {
            .type = NodeType::Variable,
            .name = intern("foo.c"),
            .parent_dir = src_dir->id(),
        });
        REQUIRE(var.has_value());
        auto twin = add_file_node(g, FileNode {
            .type = NodeType::Variable,
            .name = intern("foo.c"),
            .parent_dir = src_dir->id(),
        });
        REQUIRE(twin.has_value());
        REQUIRE(find_by_dir_name(g, *src_dir, "foo.c") == first->id());
    }
}

TEST_CASE("add_file_node refuses a parent that is not a directory node", "[graph][path_pool]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;

    auto src_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("src") });
    REQUIRE(src_dir.has_value());
    auto root_foo = add_file_node(g, FileNode { .name = intern("foo.c") });
    REQUIRE(root_foo.has_value());
    auto const next_id = g.next_file_id;

    SECTION("an id no node was minted under is an error")
    {
        auto orphan = add_file_node(g, FileNode { .name = intern("bar.c"), .parent_dir = pup::NodeId { 999 } });
        REQUIRE_FALSE(orphan.has_value());
        REQUIRE(orphan.error().code == pup::ErrorCode::InvalidNodeId);
        REQUIRE(g.next_file_id == next_id);
        REQUIRE(find_by_dir_name(g, source_root(g), "bar.c") == std::nullopt);
    }

    SECTION("a command id is an error even when its index matches a directory")
    {
        auto const cmd = pup::node_id::make_command(src_dir->index());
        auto orphan = add_file_node(g, FileNode { .name = intern("bar.c"), .parent_dir = cmd });
        REQUIRE_FALSE(orphan.has_value());
        REQUIRE(orphan.error().code == pup::ErrorCode::InvalidNodeId);
        REQUIRE(find_by_dir_name(g, *src_dir, "bar.c") == std::nullopt);
    }

    SECTION("a parent that is a file rather than a directory is an error naming what it is")
    {
        auto child = add_file_node(g, FileNode { .name = intern("x.o"), .parent_dir = root_foo->id() });
        REQUIRE_FALSE(child.has_value());
        REQUIRE(child.error().code == pup::ErrorCode::InvalidNodeId);
        REQUIRE(pup::global_pool().get(child.error().message).find("the file 'foo.c' is not a directory") != std::string_view::npos);
        REQUIRE(g.next_file_id == next_id);
    }

    SECTION("a parent that is a group is an error")
    {
        auto group = add_file_node(g, FileNode { .type = NodeType::Group, .name = intern("<g>"), .parent_dir = src_dir->id() });
        REQUIRE(group.has_value());
        auto child = add_file_node(g, FileNode { .name = intern("x.o"), .parent_dir = group->id() });
        REQUIRE_FALSE(child.has_value());
        REQUIRE(child.error().code == pup::ErrorCode::InvalidNodeId);
    }

    SECTION("the build root is a directory a node may sit under")
    {
        auto child = add_file_node(g, FileNode { .type = NodeType::Generated, .name = intern("x.o"), .parent_dir = pup::BUILD_ROOT_ID });
        REQUIRE(child.has_value());
    }

    SECTION("the refusal leaves the source root's entry of the same name alone")
    {
        auto orphan = add_file_node(g, FileNode { .name = intern("foo.c"), .parent_dir = pup::NodeId { 999 } });
        REQUIRE_FALSE(orphan.has_value());
        REQUIRE(orphan.error().code == pup::ErrorCode::InvalidNodeId);
        REQUIRE(find_by_dir_name(g, source_root(g), "foo.c") == root_foo->id());
    }
}

TEST_CASE("FileNode path_id populated by add_file_node", "[graph][path_pool]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;

    auto src_dir = add_file_node(g, FileNode { .type = NodeType::Directory, .name = intern("src") });
    auto foo = add_file_node(g, FileNode { .name = intern("foo.c"), .parent_dir = src_dir->id() });

    REQUIRE(src_dir.has_value());
    REQUIRE(foo.has_value());

    SECTION("directory node has SourceRoot-grounded path_id")
    {
        auto const* node = get_file_node(g, src_dir->id());
        REQUIRE(node != nullptr);
        REQUIRE_FALSE(pup::is_root(node->path_id));
        REQUIRE(g.paths.name(node->path_id) == intern("src"));
        REQUIRE(g.paths.parent(node->path_id) == pup::PathId::SourceRoot);
        REQUIRE(g.paths.root(node->path_id) == pup::PathId::SourceRoot);
    }

    SECTION("file node has chained path_id")
    {
        auto const* node = get_file_node(g, foo->id());
        auto const* parent = get_file_node(g, src_dir->id());
        REQUIRE(node != nullptr);
        REQUIRE(parent != nullptr);
        REQUIRE(g.paths.name(node->path_id) == intern("foo.c"));
        REQUIRE(g.paths.parent(node->path_id) == parent->path_id);
    }

    SECTION("path_to_node resolves PathId back to NodeId")
    {
        auto const* node = get_file_node(g, foo->id());
        REQUIRE(node != nullptr);
        auto const* resolved = g.path_to_node.find(pup::to_underlying(node->path_id));
        REQUIRE(resolved != nullptr);
        REQUIRE(static_cast<pup::NodeId>(*resolved) == foo->id());
    }

    SECTION("to_string round-trips through path_id")
    {
        auto const* node = get_file_node(g, foo->id());
        REQUIRE(node != nullptr);
        auto& pool = pup::global_pool();
        REQUIRE(sv(g.paths.to_string(node->path_id, pool)) == "src/foo.c");
    }
}

TEST_CASE("ensure_file_node returns the source node an in-tree build path aliases", "[graph]")
{
    auto bs = make_build_graph();
    auto& graph = bs.graph;
    auto& pool = pup::global_pool();

    auto build_twin = graph.paths.intern_path("d/f", pool, pup::PathId::BuildRoot);
    auto source_file = graph.paths.intern_path("d/f", pool, pup::PathId::SourceRoot);
    auto file_id = ensure_file_node(graph, source_file, NodeType::File);
    REQUIRE(file_id.has_value());

    auto aliased = ensure_file_node(graph, build_twin, NodeType::Directory);

    REQUIRE(aliased.has_value());
    REQUIRE(*aliased == *file_id);
    REQUIRE(*graph.path_to_node.find(pup::to_underlying(build_twin)) == file_id->id());
}

TEST_CASE("An in-tree output lands in the source directory it names rather than a second one", "[graph]")
{
    auto bs = make_build_graph();
    auto& graph = bs.graph;
    auto& pool = pup::global_pool();

    auto source_dir = ensure_file_node(graph, graph.paths.intern_path("d", pool, pup::PathId::SourceRoot), NodeType::Directory);
    REQUIRE(source_dir.has_value());

    auto output = ensure_file_node(graph, graph.paths.intern_path("d/out.o", pool, pup::PathId::BuildRoot), NodeType::Generated);
    REQUIRE(output.has_value());

    REQUIRE(get_parent_dir(graph, *output) == *source_dir);
    REQUIRE(ensure_file_node(graph, graph.paths.intern_path("d", pool, pup::PathId::BuildRoot), NodeType::Directory) == *source_dir);
    REQUIRE(ensure_file_node(graph, pup::PathId::BuildRoot, NodeType::Directory) == source_root(graph));
}

TEST_CASE("The build root resolves to one node whether looked up by path or ensured", "[graph]")
{
    auto bs = make_build_graph();
    auto& graph = bs.graph;
    auto const by_path = [&] { return pup::NodeId { *graph.path_to_node.find(pup::to_underlying(pup::PathId::BuildRoot)) }; };

    REQUIRE(by_path() == pup::SOURCE_ROOT_ID);
    REQUIRE(ensure_file_node(graph, pup::PathId::BuildRoot, NodeType::Directory)->id() == by_path());

    REQUIRE(set_build_root_name(graph, "build").has_value());
    REQUIRE(by_path() == pup::BUILD_ROOT_ID);
    REQUIRE(ensure_file_node(graph, pup::PathId::BuildRoot, NodeType::Directory)->id() == by_path());

    REQUIRE(set_build_root_name(graph, "").has_value());
    REQUIRE(by_path() == pup::SOURCE_ROOT_ID);
    REQUIRE(ensure_file_node(graph, pup::PathId::BuildRoot, NodeType::Directory)->id() == by_path());
}

TEST_CASE("The build root cannot take the name of a source directory", "[graph]")
{
    auto bs = make_build_graph();
    auto& graph = bs.graph;
    auto const existing = add_file_node(graph, FileNode { .type = NodeType::Directory, .name = intern("build") });
    REQUIRE(existing.has_value());

    auto const named = set_build_root_name(graph, "build");

    REQUIRE_FALSE(named.has_value());
    REQUIRE(named.error().code == pup::ErrorCode::DuplicateNode);
    REQUIRE(find_by_dir_name(graph, source_root(graph), "build") == existing->id());
    REQUIRE(get_build_root_name(graph).empty());
}

TEST_CASE("In an in-tree build the build root's child by name is its source twin", "[graph]")
{
    auto bs = make_build_graph();
    auto& graph = bs.graph;
    auto const twin = ensure_file_node(graph, graph.paths.intern_path("a.c", pup::global_pool(), pup::PathId::BuildRoot), NodeType::File);
    REQUIRE(twin.has_value());

    REQUIRE(find_by_dir_name(graph, source_root(graph), "a.c") == twin->id());
    REQUIRE(find_by_dir_name(graph, *find_file(graph, pup::BUILD_ROOT_ID), "a.c") == twin->id());
}

TEST_CASE("The empty fragment ensures the source root", "[graph]")
{
    auto bs = make_build_graph();
    auto& graph = bs.graph;

    REQUIRE(ensure_file_node(graph, pup::PathId::Ungrounded, NodeType::Directory) == source_root(graph));
}

TEST_CASE("ensure_file_node creates nodes from PathId", "[graph]")
{
    auto bs = make_build_graph();
    auto& graph = bs.graph;
    auto& pool = pup::global_pool();

    REQUIRE(set_build_root_name(graph, "build").has_value());

    SECTION("creates new file node under build root")
    {
        auto path_id = graph.paths.intern_path("foo.o", pool, pup::PathId::BuildRoot);
        auto result = ensure_file_node(graph, path_id, NodeType::Generated);
        REQUIRE(result.has_value());

        REQUIRE(get<NodeType>(graph,*result) == NodeType::Generated);
        REQUIRE(sv(get<Name>(graph, *result)) == "foo.o");
        REQUIRE(graph.paths.root(get_file_node(graph, result->id())->path_id) == pup::PathId::BuildRoot);
    }

    SECTION("returns existing node on second call")
    {
        auto path_id = graph.paths.intern_path("bar.o", pool, pup::PathId::BuildRoot);
        auto first = ensure_file_node(graph, path_id, NodeType::Generated);
        auto second = ensure_file_node(graph, path_id, NodeType::Generated);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        REQUIRE(*first == *second);
    }

    SECTION("creates intermediate directories")
    {
        auto path_id = graph.paths.intern_path("src/lib/baz.o", pool, pup::PathId::BuildRoot);
        auto result = ensure_file_node(graph, path_id, NodeType::Generated);
        REQUIRE(result.has_value());

        REQUIRE(sv(get<Name>(graph, *result)) == "baz.o");

        auto parent_id = get_parent_dir(graph, *result);
        REQUIRE(sv(get<Name>(graph, parent_id)) == "lib");
        REQUIRE(get<NodeType>(graph,parent_id) == NodeType::Directory);
    }

    SECTION("upgrades Ghost to Generated")
    {
        auto path_id = graph.paths.intern_path("ghost.o", pool, pup::PathId::BuildRoot);

        auto ghost_result = ensure_file_node(graph, path_id, NodeType::Ghost);
        REQUIRE(ghost_result.has_value());
        REQUIRE(get<NodeType>(graph,*ghost_result) == NodeType::Ghost);

        auto gen_result = ensure_file_node(graph, path_id, NodeType::Generated);
        REQUIRE(gen_result.has_value());
        REQUIRE(*gen_result == *ghost_result);
        REQUIRE(get<NodeType>(graph,*gen_result) == NodeType::Generated);
    }

    SECTION("path_to_node is consistent after creation")
    {
        auto path_id = graph.paths.intern_path("check.o", pool, pup::PathId::BuildRoot);
        auto result = ensure_file_node(graph, path_id, NodeType::Generated);
        REQUIRE(result.has_value());

        auto const* found = graph.path_to_node.find(pup::to_underlying(path_id));
        REQUIRE(found != nullptr);
        REQUIRE(*found == result->id());
    }

    SECTION("BuildRoot PathId returns BUILD_ROOT_ID")
    {
        auto result = ensure_file_node(graph, pup::PathId::BuildRoot, NodeType::Directory);
        REQUIRE(result.has_value());
        REQUIRE(result->id() == pup::BUILD_ROOT_ID);
    }

    SECTION("ungrounded PathId resolves to existing BuildRoot node")
    {
        auto grounded = graph.paths.intern_path("obj/main.o", pool, pup::PathId::BuildRoot);
        auto existing = ensure_file_node(graph, grounded, NodeType::Generated);
        REQUIRE(existing.has_value());

        auto ungrounded = graph.paths.intern_path("obj/main.o", pool, pup::PathId::Ungrounded);
        REQUIRE(!graph.paths.is_grounded(ungrounded));

        auto resolved = ensure_file_node(graph, ungrounded, NodeType::Generated);
        REQUIRE(resolved.has_value());
        REQUIRE(*resolved == *existing);

        auto const* node = get_file_node(graph, resolved->id());
        REQUIRE(node != nullptr);
        REQUIRE(graph.paths.is_grounded(node->path_id));
        REQUIRE(graph.paths.root(node->path_id) == pup::PathId::BuildRoot);
    }

    SECTION("ungrounded PathId resolves to existing SourceRoot node")
    {
        auto grounded = graph.paths.intern_path("input.c", pool, pup::PathId::SourceRoot);
        auto existing = ensure_file_node(graph, grounded, NodeType::File);
        REQUIRE(existing.has_value());

        auto ungrounded = graph.paths.intern_path("input.c", pool, pup::PathId::Ungrounded);
        auto resolved = ensure_file_node(graph, ungrounded, NodeType::File);
        REQUIRE(resolved.has_value());
        REQUIRE(*resolved == *existing);

        auto const* node = get_file_node(graph, resolved->id());
        REQUIRE(node != nullptr);
        REQUIRE(graph.paths.root(node->path_id) == pup::PathId::SourceRoot);
    }

    SECTION("ungrounded Ghost creates under BuildRoot")
    {
        auto ungrounded = graph.paths.intern_path("gen/output.o", pool, pup::PathId::Ungrounded);
        REQUIRE(!graph.paths.is_grounded(ungrounded));

        auto result = ensure_file_node(graph, ungrounded, NodeType::Ghost);
        REQUIRE(result.has_value());

        auto const* node = get_file_node(graph, result->id());
        REQUIRE(node != nullptr);
        REQUIRE(node->type == NodeType::Ghost);
        REQUIRE(graph.paths.is_grounded(node->path_id));
        REQUIRE(graph.paths.root(node->path_id) == pup::PathId::BuildRoot);
    }

    SECTION("ungrounded Generated creates under BuildRoot")
    {
        auto ungrounded = graph.paths.intern_path("out/lib.a", pool, pup::PathId::Ungrounded);
        auto result = ensure_file_node(graph, ungrounded, NodeType::Generated);
        REQUIRE(result.has_value());

        auto const* node = get_file_node(graph, result->id());
        REQUIRE(node != nullptr);
        REQUIRE(node->type == NodeType::Generated);
        REQUIRE(graph.paths.is_grounded(node->path_id));
        REQUIRE(graph.paths.root(node->path_id) == pup::PathId::BuildRoot);
    }

    SECTION("ungrounded File creates under SourceRoot")
    {
        auto ungrounded = graph.paths.intern_path("src/foo.c", pool, pup::PathId::Ungrounded);
        auto result = ensure_file_node(graph, ungrounded, NodeType::File);
        REQUIRE(result.has_value());

        auto const* node = get_file_node(graph, result->id());
        REQUIRE(node != nullptr);
        REQUIRE(node->type == NodeType::File);
        REQUIRE(graph.paths.is_grounded(node->path_id));
        REQUIRE(graph.paths.root(node->path_id) == pup::PathId::SourceRoot);
    }

    SECTION("ungrounded Generated upgrades existing BuildRoot Ghost")
    {
        auto grounded = graph.paths.intern_path("dep.o", pool, pup::PathId::BuildRoot);
        auto ghost = ensure_file_node(graph, grounded, NodeType::Ghost);
        REQUIRE(ghost.has_value());
        REQUIRE(get<NodeType>(graph,*ghost) == NodeType::Ghost);

        auto ungrounded = graph.paths.intern_path("dep.o", pool, pup::PathId::Ungrounded);
        auto gen = ensure_file_node(graph, ungrounded, NodeType::Generated);
        REQUIRE(gen.has_value());
        REQUIRE(*gen == *ghost);
        REQUIRE(get<NodeType>(graph,*gen) == NodeType::Generated);
    }

    SECTION("ungrounded prefers BuildRoot over SourceRoot for lookup")
    {
        auto build_id = graph.paths.intern_path("shared.h", pool, pup::PathId::BuildRoot);
        auto build_node = ensure_file_node(graph, build_id, NodeType::Generated);
        REQUIRE(build_node.has_value());

        auto source_id = graph.paths.intern_path("shared.h", pool, pup::PathId::SourceRoot);
        auto source_node = ensure_file_node(graph, source_id, NodeType::File);
        REQUIRE(source_node.has_value());
        REQUIRE(*build_node != *source_node);

        auto ungrounded = graph.paths.intern_path("shared.h", pool, pup::PathId::Ungrounded);
        auto resolved = ensure_file_node(graph, ungrounded, NodeType::Generated);
        REQUIRE(resolved.has_value());
        REQUIRE(*resolved == *build_node);
    }
}

TEST_CASE("node_id encoding: 30-bit index roundtrips and kinds are mutually exclusive", "[graph]")
{
    auto const max_index = std::size_t { 0x3FFFFFFF };

    for (auto idx : { std::size_t { 1 }, std::size_t { 12345 }, max_index }) {
        auto file = pup::NodeId { static_cast<pup::NodeId>(idx) };
        auto cmd = pup::node_id::make_command(idx);
        auto cond = pup::node_id::make_condition(idx);
        auto phi = pup::node_id::make_phi(idx);

        REQUIRE(pup::node_id::index(file) == idx);
        REQUIRE(pup::node_id::index(cmd) == idx);
        REQUIRE(pup::node_id::index(cond) == idx);
        REQUIRE(pup::node_id::index(phi) == idx);

        REQUIRE(pup::node_id::is_file(file));
        REQUIRE_FALSE(pup::node_id::is_command(file));
        REQUIRE_FALSE(pup::node_id::is_condition(file));
        REQUIRE_FALSE(pup::node_id::is_phi(file));

        REQUIRE(pup::node_id::is_command(cmd));
        REQUIRE_FALSE(pup::node_id::is_file(cmd));
        REQUIRE_FALSE(pup::node_id::is_condition(cmd));
        REQUIRE_FALSE(pup::node_id::is_phi(cmd));

        REQUIRE(pup::node_id::is_condition(cond));
        REQUIRE_FALSE(pup::node_id::is_file(cond));
        REQUIRE_FALSE(pup::node_id::is_command(cond));
        REQUIRE_FALSE(pup::node_id::is_phi(cond));

        REQUIRE(pup::node_id::is_phi(phi));
        REQUIRE_FALSE(pup::node_id::is_file(phi));
        REQUIRE_FALSE(pup::node_id::is_command(phi));
        REQUIRE_FALSE(pup::node_id::is_condition(phi));
    }

    REQUIRE(pup::node_id::is_file(pup::SOURCE_ROOT_ID));
}

TEST_CASE("The source root is a live directory node that enumeration skips", "[graph]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;
    auto file = add_file_node(g, FileNode { .name = intern("main.c") });
    REQUIRE(file.has_value());

    auto const* root = get_file_node(g, pup::SOURCE_ROOT_ID);
    REQUIRE(root != nullptr);
    CHECK(root->type == NodeType::Directory);
    CHECK(root->path_id == pup::PathId::SourceRoot);
    CHECK(get_parent_dir(g, *file) == source_root(g));
    CHECK(sv(get_full_path(g, source_root(g))).empty());
    auto const nodes = all_nodes(g);
    CHECK(std::ranges::find(nodes, pup::SOURCE_ROOT_ID) == nodes.end());
    auto const directories = pup::test::files_of_type(g, NodeType::Directory);
    CHECK(std::ranges::find(directories, source_root(g)) == directories.end());
}

TEST_CASE("files_for_each visits the file endpoints of matching edges in order", "[graph]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;
    auto first = add_file_node(g, FileNode { .name = intern("a.c") });
    REQUIRE(first.has_value());
    auto second = add_file_node(g, FileNode { .name = intern("b.c") });
    REQUIRE(second.has_value());
    auto sticky = add_file_node(g, FileNode { .name = intern("c.c") });
    REQUIRE(sticky.has_value());
    auto cmd = add_command_node(g, CommandNode {});
    REQUIRE(cmd.has_value());
    auto scan = add_command_node(g, CommandNode {});
    REQUIRE(scan.has_value());
    REQUIRE(add_edge(g, cmd->id(), second->id()));
    REQUIRE(add_edge(g, cmd->id(), scan->id()));
    REQUIRE(add_edge(g, cmd->id(), first->id()));
    REQUIRE(add_edge(g, cmd->id(), sticky->id(), LinkType::Sticky));

    auto visited = std::vector<FileId> {};
    files_for_each(g, cmd->id(), EdgeDirection::Forward, edge_mask::data_flow, [&](FileId file) { visited.push_back(file); });

    REQUIRE(visited.size() == 2);
    CHECK(visited[0] == *second);
    CHECK(visited[1] == *first);
}

TEST_CASE("An id that names no node resolves to nothing", "[graph]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;
    auto file = add_file_node(g, FileNode { .name = intern("main.c") });
    REQUIRE(file.has_value());
    auto cmd = add_command_node(g, CommandNode {});
    REQUIRE(cmd.has_value());

    SECTION("a live id resolves to its own handle")
    {
        auto const found_file = find_file(g, file->id());
        REQUIRE(found_file.has_value());
        CHECK(*found_file == *file);
        auto const found_cmd = find_command(g, cmd->id());
        REQUIRE(found_cmd.has_value());
        CHECK(*found_cmd == *cmd);
    }

    SECTION("an out-of-range id")
    {
        CHECK_FALSE(find_file(g, pup::NodeId { 1000 }).has_value());
        CHECK_FALSE(find_command(g, pup::node_id::make_command(1000)).has_value());
        CHECK_FALSE(find_command(g, pup::node_id::make_command(0)).has_value());
    }

    SECTION("a wrong-kind id")
    {
        CHECK_FALSE(find_file(g, cmd->id()).has_value());
        CHECK_FALSE(find_command(g, file->id()).has_value());
        CHECK_FALSE(find_file(g, pup::node_id::make_condition(file->index())).has_value());
    }

    SECTION("a phi-tagged id")
    {
        CHECK_FALSE(find_file(g, pup::node_id::make_phi(file->index())).has_value());
        CHECK_FALSE(find_command(g, pup::node_id::make_phi(cmd->index())).has_value());
    }
}

TEST_CASE("compute_command_key separates rules by directory", "[graph][identity]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;

    // Command text is Tupfile-relative, so identical rules in sibling directories
    // render the same string; identity must still tell them apart.
    auto in_a = add_command_node(g, CommandNode {
        .source_dir = intern("a"),
        .instruction = pup::test::instruction("cat src.txt > out.txt"),
    });
    auto in_b = add_command_node(g, CommandNode {
        .source_dir = intern("b"),
        .instruction = pup::test::instruction("cat src.txt > out.txt"),
    });
    REQUIRE(in_a.has_value());
    REQUIRE(in_b.has_value());

    SECTION("same text in different directories yields different keys")
    {
        CHECK(compute_command_key(g, *in_a, bs.path_cache)
            != compute_command_key(g, *in_b, bs.path_cache));
    }

    SECTION("the key is stable for the same command")
    {
        CHECK(compute_command_key(g, *in_a, bs.path_cache)
            == compute_command_key(g, *in_a, bs.path_cache));
    }

    SECTION("different text in the same directory yields different keys")
    {
        auto other = add_command_node(g, CommandNode {
            .source_dir = intern("a"),
            .instruction = pup::test::instruction("cat other.txt > out.txt"),
        });
        REQUIRE(other.has_value());
        CHECK(compute_command_key(g, *in_a, bs.path_cache)
            != compute_command_key(g, *other, bs.path_cache));
    }
}

TEST_CASE("Display text stays out of command identity", "[graph][identity][display]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;

    // A display annotation is presentation only: were it folded in, capturing one would restate
    // every annotated command's identity and rebuild every project carrying one.
    auto plain = add_command_node(g, CommandNode {
        .source_dir = intern("a"),
        .instruction = pup::test::instruction("cc -c main.c -o main.o"),
    });
    auto annotated = add_command_node(g, CommandNode {
        .display = intern("CC main.o"),
        .source_dir = intern("a"),
        .instruction = pup::test::instruction("cc -c main.c -o main.o"),
    });
    REQUIRE(plain.has_value());
    REQUIRE(annotated.has_value());

    SECTION("the key ignores it")
    {
        CHECK(compute_command_key(g, *plain, bs.path_cache)
            == compute_command_key(g, *annotated, bs.path_cache));
    }

    SECTION("the signature ignores it")
    {
        CHECK(compute_command_signature(g, *plain, bs.path_cache)
            == compute_command_signature(g, *annotated, bs.path_cache));
    }
}

TEST_CASE("compute_command_key separates dep-scan commands by parent", "[graph][identity]")
{
    auto bs = make_build_graph();
    auto& g = bs.graph;

    // A dep-scan command drops its parent's -o, so two compiles of one source with
    // equal flags render byte-identical scans; only the parent tells them apart.
    auto compile_a = add_command_node(g, CommandNode {
        .source_dir = intern("a"),
        .instruction = pup::test::instruction("g++ -c ggc.cc -o one.o"),
    });
    auto compile_b = add_command_node(g, CommandNode {
        .source_dir = intern("a"),
        .instruction = pup::test::instruction("g++ -c ggc.cc -o two.o"),
    });
    REQUIRE(compile_a.has_value());
    REQUIRE(compile_b.has_value());

    auto scan_a = add_command_node(g, CommandNode {
        .source_dir = intern("a"),
        .instruction = pup::test::instruction("g++ -M ggc.cc"),
        .output_action = OutputAction::InjectImplicitDeps,
        .parent_command = compile_a->id(),
    });
    auto scan_b = add_command_node(g, CommandNode {
        .source_dir = intern("a"),
        .instruction = pup::test::instruction("g++ -M ggc.cc"),
        .output_action = OutputAction::InjectImplicitDeps,
        .parent_command = compile_b->id(),
    });
    REQUIRE(scan_a.has_value());
    REQUIRE(scan_b.has_value());

    SECTION("identical scan text under different parents yields different keys")
    {
        CHECK(compute_command_key(g, *scan_a, bs.path_cache)
            != compute_command_key(g, *scan_b, bs.path_cache));
    }

    SECTION("a scan's key is stable")
    {
        CHECK(compute_command_key(g, *scan_a, bs.path_cache)
            == compute_command_key(g, *scan_a, bs.path_cache));
    }
}
