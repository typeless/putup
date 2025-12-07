// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "catch_amalgamated.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/topo.hpp"

using namespace pup::graph;
using pup::NodeType;

TEST_CASE("BuildGraph basic operations", "[graph]")
{
    auto graph = BuildGraph{};

    SECTION("add nodes")
    {
        auto node1 = Node{.type = NodeType::File, .path = "foo.c"};
        auto node2 = Node{.type = NodeType::File, .path = "bar.c"};

        auto id1 = graph.add_node(node1);
        auto id2 = graph.add_node(node2);

        REQUIRE(id1.has_value());
        REQUIRE(id2.has_value());
        REQUIRE(*id1 != *id2);

        REQUIRE(graph.node_count() == 2);
    }

    SECTION("find by path")
    {
        auto node = Node{.type = NodeType::File, .path = "src/foo.c"};
        auto id = graph.add_node(node);

        REQUIRE(id.has_value());
        auto found = graph.find_by_path("src/foo.c");
        REQUIRE(found.has_value());
        REQUIRE(*found == *id);

        REQUIRE_FALSE(graph.find_by_path("nonexistent.c").has_value());
    }

    SECTION("add edges")
    {
        auto id1 = graph.add_node(Node{.type = NodeType::File, .path = "foo.c"});
        auto id2 = graph.add_node(Node{.type = NodeType::Command, .command = "gcc foo.c"});
        auto id3 = graph.add_node(Node{.type = NodeType::Generated, .path = "foo.o"});

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
        (void)graph.add_node(Node{.type = NodeType::File, .path = "a.c"});
        (void)graph.add_node(Node{.type = NodeType::File, .path = "b.c"});
        (void)graph.add_node(Node{.type = NodeType::Command, .command = "gcc"});
        (void)graph.add_node(Node{.type = NodeType::Generated, .path = "out.o"});

        auto files = graph.nodes_of_type(NodeType::File);
        REQUIRE(files.size() == 2);

        auto commands = graph.nodes_of_type(NodeType::Command);
        REQUIRE(commands.size() == 1);

        auto generated = graph.nodes_of_type(NodeType::Generated);
        REQUIRE(generated.size() == 1);
    }

    SECTION("root and leaf nodes")
    {
        auto id1 = graph.add_node(Node{.type = NodeType::File, .path = "a.c"});
        auto id2 = graph.add_node(Node{.type = NodeType::Command});
        auto id3 = graph.add_node(Node{.type = NodeType::Generated, .path = "a.o"});

        (void)graph.add_edge(*id1, *id2);
        (void)graph.add_edge(*id2, *id3);

        auto roots = graph.root_nodes();
        REQUIRE(roots.size() == 1);
        REQUIRE(roots[0] == *id1);

        auto leaves = graph.leaf_nodes();
        REQUIRE(leaves.size() == 1);
        REQUIRE(leaves[0] == *id3);
    }
}

TEST_CASE("Topological sort", "[graph]")
{
    auto graph = BuildGraph{};

    SECTION("simple linear graph")
    {
        auto id1 = graph.add_node(Node{.path = "a"});
        auto id2 = graph.add_node(Node{.path = "b"});
        auto id3 = graph.add_node(Node{.path = "c"});

        (void)graph.add_edge(*id1, *id2);
        (void)graph.add_edge(*id2, *id3);

        auto result = topological_sort(graph);
        REQUIRE_FALSE(result.has_cycle);
        REQUIRE(result.order.size() == 3);

        // a should come before b, b before c
        auto pos_a = std::find(result.order.begin(), result.order.end(), *id1);
        auto pos_b = std::find(result.order.begin(), result.order.end(), *id2);
        auto pos_c = std::find(result.order.begin(), result.order.end(), *id3);

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
        auto id_a = graph.add_node(Node{.path = "a"});
        auto id_b = graph.add_node(Node{.path = "b"});
        auto id_c = graph.add_node(Node{.path = "c"});
        auto id_d = graph.add_node(Node{.path = "d"});

        (void)graph.add_edge(*id_a, *id_b);
        (void)graph.add_edge(*id_a, *id_c);
        (void)graph.add_edge(*id_b, *id_d);
        (void)graph.add_edge(*id_c, *id_d);

        auto result = topological_sort(graph);
        REQUIRE_FALSE(result.has_cycle);
        REQUIRE(result.order.size() == 4);

        // a should come first, d should come last
        REQUIRE(result.order[0] == *id_a);
        REQUIRE(result.order[3] == *id_d);
    }

    SECTION("cycle detection")
    {
        auto id1 = graph.add_node(Node{.path = "a"});
        auto id2 = graph.add_node(Node{.path = "b"});
        auto id3 = graph.add_node(Node{.path = "c"});

        (void)graph.add_edge(*id1, *id2);
        (void)graph.add_edge(*id2, *id3);
        (void)graph.add_edge(*id3, *id1); // Creates cycle

        auto result = topological_sort(graph);
        REQUIRE(result.has_cycle);
        REQUIRE_FALSE(result.cycle.empty());
    }

    SECTION("is_dag")
    {
        auto id1 = graph.add_node(Node{.path = "a"});
        auto id2 = graph.add_node(Node{.path = "b"});

        (void)graph.add_edge(*id1, *id2);

        REQUIRE(is_dag(graph));

        (void)graph.add_edge(*id2, *id1);
        REQUIRE_FALSE(is_dag(graph));
    }
}

TEST_CASE("BuildGraph node types", "[graph]")
{
    auto graph = BuildGraph{};

    SECTION("Group node type")
    {
        auto node = Node{.type = NodeType::Group, .path = "{objs}"};
        auto id = graph.add_node(node);

        REQUIRE(id.has_value());
        auto const* n = graph.get_node(*id);
        REQUIRE(n != nullptr);
        REQUIRE(n->type == NodeType::Group);
    }

    SECTION("all node types")
    {
        auto file_id = graph.add_node(Node{.type = NodeType::File, .path = "a.c"});
        auto cmd_id = graph.add_node(Node{.type = NodeType::Command, .command = "gcc"});
        auto gen_id = graph.add_node(Node{.type = NodeType::Generated, .path = "a.o"});
        auto dir_id = graph.add_node(Node{.type = NodeType::Directory, .path = "src"});
        auto var_id = graph.add_node(Node{.type = NodeType::Variable, .path = "CC"});
        auto group_id = graph.add_node(Node{.type = NodeType::Group, .path = "{objs}"});
        auto gen_dir_id = graph.add_node(Node{.type = NodeType::GeneratedDir, .path = "build"});

        REQUIRE(file_id.has_value());
        REQUIRE(cmd_id.has_value());
        REQUIRE(gen_id.has_value());
        REQUIRE(dir_id.has_value());
        REQUIRE(var_id.has_value());
        REQUIRE(group_id.has_value());
        REQUIRE(gen_dir_id.has_value());

        REQUIRE(graph.node_count() == 7);
        REQUIRE(graph.nodes_of_type(NodeType::File).size() == 1);
        REQUIRE(graph.nodes_of_type(NodeType::Command).size() == 1);
        REQUIRE(graph.nodes_of_type(NodeType::Generated).size() == 1);
        REQUIRE(graph.nodes_of_type(NodeType::Group).size() == 1);
    }
}

TEST_CASE("BuildGraph edge types", "[graph]")
{
    auto graph = BuildGraph{};

    SECTION("order-only edges")
    {
        auto id1 = graph.add_node(Node{.type = NodeType::File, .path = "header.h"});
        auto id2 = graph.add_node(Node{.type = NodeType::Command, .command = "gcc"});

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
        auto cmd_id = graph.add_node(Node{.type = NodeType::Command, .command = "gcc"});
        auto out_id = graph.add_node(Node{.type = NodeType::Generated, .path = "a.o"});
        auto group_id = graph.add_node(Node{.type = NodeType::Group, .path = "{objs}"});

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
    auto graph = BuildGraph{};

    //     a
    //    / |
    //   b   c
    //   |   |
    //   d   e
    auto id_a = graph.add_node(Node{.path = "a"});
    auto id_b = graph.add_node(Node{.path = "b"});
    auto id_c = graph.add_node(Node{.path = "c"});
    auto id_d = graph.add_node(Node{.path = "d"});
    auto id_e = graph.add_node(Node{.path = "e"});

    (void)graph.add_edge(*id_a, *id_b);
    (void)graph.add_edge(*id_a, *id_c);
    (void)graph.add_edge(*id_b, *id_d);
    (void)graph.add_edge(*id_c, *id_e);

    SECTION("reachable_from")
    {
        auto reachable = reachable_from(graph, *id_a);
        REQUIRE(reachable.size() == 5);

        reachable = reachable_from(graph, *id_b);
        REQUIRE(reachable.size() == 2); // b, d
    }

    SECTION("can_reach")
    {
        auto reaching = can_reach(graph, *id_d);
        REQUIRE(reaching.size() == 3); // d, b, a
    }

    SECTION("has_path")
    {
        REQUIRE(has_path(graph, *id_a, *id_d));
        REQUIRE(has_path(graph, *id_a, *id_e));
        REQUIRE_FALSE(has_path(graph, *id_d, *id_a));
        REQUIRE_FALSE(has_path(graph, *id_b, *id_c));
    }

    SECTION("node_depth")
    {
        REQUIRE(node_depth(graph, *id_a) == 0);
        REQUIRE(node_depth(graph, *id_b) == 1);
        REQUIRE(node_depth(graph, *id_c) == 1);
        REQUIRE(node_depth(graph, *id_d) == 2);
        REQUIRE(node_depth(graph, *id_e) == 2);
    }

    SECTION("max_depth")
    {
        REQUIRE(max_depth(graph) == 2);
    }

    SECTION("nodes_at_depth")
    {
        auto depth0 = nodes_at_depth(graph, 0);
        REQUIRE(depth0.size() == 1);
        REQUIRE(depth0[0] == *id_a);

        auto depth1 = nodes_at_depth(graph, 1);
        REQUIRE(depth1.size() == 2);

        auto depth2 = nodes_at_depth(graph, 2);
        REQUIRE(depth2.size() == 2);
    }

    SECTION("critical_path")
    {
        auto path = critical_path(graph);
        REQUIRE(path.size() == 3);
        REQUIRE(path[0] == *id_a);
    }
}

TEST_CASE("Order-only dependencies in topological sort", "[graph]")
{
    auto graph = BuildGraph{};

    // Build a graph where:
    // - header.h is an order-only dependency of cmd
    // - input.c is a normal input to cmd
    // - cmd produces output.o
    //
    // header.h ---(order-only)---> cmd ---> output.o
    //                               ^
    // input.c ----------------------+

    auto header_id = graph.add_node(Node{.type = NodeType::File, .path = "header.h"});
    auto input_id = graph.add_node(Node{.type = NodeType::File, .path = "input.c"});
    auto cmd_id = graph.add_node(Node{.type = NodeType::Command, .command = "gcc"});
    auto output_id = graph.add_node(Node{.type = NodeType::Generated, .path = "output.o"});

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
