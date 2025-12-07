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
        //    / \
        //   b   c
        //    \ /
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

TEST_CASE("Graph traversal", "[graph]")
{
    auto graph = BuildGraph{};

    //     a
    //    / \
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
