// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/result.hpp"
#include "pup/core/types.hpp"
#include "pup/graph/rule_pattern.hpp"

#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pup::graph {

/// Edge between nodes in the build graph
struct Edge {
    NodeId from = 0;
    NodeId to = 0;
    LinkType type = LinkType::Normal;
    NodeId group_cmd_id = 0; ///< For group edges, the command that produced the group
};

/// Node in the build graph
struct Node {
    NodeId id = 0;
    NodeType type = NodeType::File;
    NodeFlags flags = NodeFlags::None;

    std::string name = {};       ///< Basename only (tup-style identification)
    std::string command = {};    ///< For commands: the command string
    std::string display = {};    ///< For commands: display text (from ^ ^ markers)
    std::string source_dir = {}; ///< For commands: Tupfile directory (relative to root)

    NodeId parent_dir = 0;         ///< Parent directory node (used with name for lookup)
    Hash256 content_hash = { {} }; ///< Content hash for files (double braces force zero-init)

    std::vector<NodeId> inputs = {};          ///< Input edges (all types)
    std::vector<NodeId> outputs = {};         ///< Output edges (non-sticky only)
    std::vector<NodeId> sticky_outputs = {};  ///< Sticky edges (Tupfile/config deps)
    std::vector<NodeId> order_only = {};      ///< Order-only dependencies
    std::set<std::string> exported_vars = {}; ///< Env vars to export to command

    // For generated rules (auto-generated from pattern matching)
    std::optional<GeneratedOutput> generated_output = {}; ///< Output specification
    OutputAction output_action = {};                      ///< What to do with output
    NodeId parent_command = INVALID_NODE_ID;              ///< Parent command for InjectImplicitDeps
};

/// Key for directory + name lookup
struct DirNameKey {
    NodeId parent_dir = 0;
    std::string name = {};

    auto operator==(DirNameKey const& other) const -> bool = default;
};

/// Hash function for DirNameKey
struct DirNameKeyHash {
    auto operator()(DirNameKey const& key) const noexcept -> std::size_t
    {
        auto h1 = std::hash<NodeId> {}(key.parent_dir);
        auto h2 = std::hash<std::string> {}(key.name);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

/// Transparent hash for string heterogeneous lookup
struct StringHash {
    using is_transparent = void;

    auto operator()(std::string_view sv) const noexcept -> std::size_t
    {
        return std::hash<std::string_view> {}(sv);
    }

    auto operator()(std::string const& s) const noexcept -> std::size_t
    {
        return std::hash<std::string_view> {}(s);
    }
};

/// Build graph - DAG of nodes and edges (plain data struct)
struct Graph {
    std::deque<Node> files;    ///< Files, directories, groups (non-command nodes)
    std::deque<Node> commands; ///< Command nodes only
    std::vector<Edge> edges;

    std::unordered_map<DirNameKey, NodeId, DirNameKeyHash> dir_name_index;
    std::unordered_map<std::string, NodeId, StringHash, std::equal_to<>> command_str_index;
    std::unordered_map<NodeId, std::vector<NodeId>> order_only_dependents;
    mutable std::unordered_map<NodeId, std::string> path_cache;

    NodeId next_file_id = 2;                     ///< Next file node ID (starts at 2, BUILD_ROOT is 1)
    NodeId next_command_id = make_command_id(1); ///< Next command node ID
};

/// Create a new empty graph with build root initialized
[[nodiscard]]
auto make_graph() -> Graph;

/// Validate that a node ID exists in the graph
[[nodiscard]]
auto validate_node_id(Graph const& graph, NodeId id) -> bool;

/// Add a node to the graph
[[nodiscard]]
auto add_node(Graph& graph, Node node) -> Result<NodeId>;

/// Add an edge between nodes
[[nodiscard]]
auto add_edge(Graph& graph, NodeId from, NodeId to, LinkType type = LinkType::Normal) -> Result<void>;

/// Add an order-only edge (dependency that doesn't trigger rebuild)
[[nodiscard]]
auto add_order_only_edge(Graph& graph, NodeId from, NodeId to) -> Result<void>;

/// Get a node by ID (mutable)
[[nodiscard]]
auto get_node(Graph& graph, NodeId id) -> Node*;

/// Get a node by ID (const)
[[nodiscard]]
auto get_node(Graph const& graph, NodeId id) -> Node const*;

/// Find a node by parent directory and basename (tup-style lookup)
[[nodiscard]]
auto find_by_dir_name(
    Graph const& graph,
    NodeId parent_dir,
    std::string_view name
) -> std::optional<NodeId>;

/// Find a node by command string
[[nodiscard]]
auto find_by_command(Graph const& graph, std::string_view cmd) -> std::optional<NodeId>;

/// Find a node by path (walks path components using find_by_dir_name)
/// Starts from SOURCE_ROOT_ID (0) by default
[[nodiscard]]
auto find_by_path(Graph const& graph, std::string_view path) -> std::optional<NodeId>;

/// Find a node by path starting from a specific root node
[[nodiscard]]
auto find_by_path(Graph const& graph, std::string_view path, NodeId root) -> std::optional<NodeId>;

/// Get all nodes of a given type
[[nodiscard]]
auto nodes_of_type(Graph const& graph, NodeType type) -> std::vector<NodeId>;

/// Get direct dependencies of a node
[[nodiscard]]
auto get_inputs(Graph const& graph, NodeId id) -> std::vector<NodeId>;

/// Get direct dependents of a node (excludes sticky edges)
/// For build-time dependency traversal, use this function.
[[nodiscard]]
auto get_outputs(Graph const& graph, NodeId id) -> std::vector<NodeId>;

/// Get sticky dependents of a node (Tupfile/config dependencies)
/// Sticky edges are parse-time dependencies - use for reparse decisions, not rebuilds.
[[nodiscard]]
auto get_sticky_outputs(Graph const& graph, NodeId id) -> std::vector<NodeId>;

/// Get order-only dependencies
[[nodiscard]]
auto get_order_only(Graph const& graph, NodeId id) -> std::vector<NodeId>;

/// Get nodes that have this node as an order-only dependency
[[nodiscard]]
auto get_order_only_dependents(Graph const& graph, NodeId id) -> std::vector<NodeId>;

/// Get total number of nodes
[[nodiscard]]
auto node_count(Graph const& graph) -> std::size_t;

/// Get total number of edges
[[nodiscard]]
auto edge_count(Graph const& graph) -> std::size_t;

/// Check if graph is empty
[[nodiscard]]
auto empty(Graph const& graph) -> bool;

/// Clear the graph
auto clear(Graph& graph) -> void;

/// Get all node IDs
[[nodiscard]]
auto all_nodes(Graph const& graph) -> std::vector<NodeId>;

/// Get root nodes (nodes with no inputs)
[[nodiscard]]
auto root_nodes(Graph const& graph) -> std::vector<NodeId>;

/// Get leaf nodes (nodes with no outputs)
[[nodiscard]]
auto leaf_nodes(Graph const& graph) -> std::vector<NodeId>;

/// Reconstruct full path from (parent_dir, name) chain
/// Uses internal cache for efficiency.
[[nodiscard]]
auto get_full_path(Graph const& graph, NodeId id) -> std::string;

/// Invalidate path cache for a node (call when parent_dir or name changes)
auto invalidate_path_cache(Graph& graph, NodeId id) -> void;

/// Clear the entire path cache
auto clear_path_cache(Graph& graph) -> void;

/// Set the build root name (relative path from source root to build root)
/// For in-tree builds, this should be empty. For variant builds, e.g. "build".
auto set_build_root_name(Graph& graph, std::string name) -> void;

/// Get the build root name
[[nodiscard]]
auto get_build_root_name(Graph const& graph) -> std::string_view;

/// Check if a node is under the build root (Generated/Ghost files)
[[nodiscard]]
auto is_under_build_root(Graph const& graph, NodeId id) -> bool;

// =============================================================================
// Legacy class wrapper (for gradual migration)
// =============================================================================

/// Build graph class wrapper - wraps Graph struct for API compatibility
/// This wrapper provides method syntax for code that hasn't been migrated yet.
class BuildGraph {
public:
    BuildGraph();
    ~BuildGraph();

    BuildGraph(BuildGraph const&) = delete;
    auto operator=(BuildGraph const&) -> BuildGraph& = delete;

    BuildGraph(BuildGraph&&) noexcept;
    auto operator=(BuildGraph&&) noexcept -> BuildGraph&;

    [[nodiscard]]
    auto add_node(Node node) -> Result<NodeId>
    {
        return graph::add_node(graph_, std::move(node));
    }

    [[nodiscard]]
    auto add_edge(NodeId from, NodeId to, LinkType type = LinkType::Normal) -> Result<void>
    {
        return graph::add_edge(graph_, from, to, type);
    }

    [[nodiscard]]
    auto add_order_only_edge(NodeId from, NodeId to) -> Result<void>
    {
        return graph::add_order_only_edge(graph_, from, to);
    }

    [[nodiscard]]
    auto get_node(NodeId id) -> Node*
    {
        return graph::get_node(graph_, id);
    }

    [[nodiscard]]
    auto get_node(NodeId id) const -> Node const*
    {
        return graph::get_node(graph_, id);
    }

    [[nodiscard]]
    auto find_by_dir_name(NodeId parent_dir, std::string_view name) const -> std::optional<NodeId>
    {
        return graph::find_by_dir_name(graph_, parent_dir, name);
    }

    [[nodiscard]]
    auto find_by_command(std::string_view cmd) const -> std::optional<NodeId>
    {
        return graph::find_by_command(graph_, cmd);
    }

    [[nodiscard]]
    auto find_by_path(std::string_view path) const -> std::optional<NodeId>
    {
        return graph::find_by_path(graph_, path);
    }

    [[nodiscard]]
    auto find_by_path(std::string_view path, NodeId root) const -> std::optional<NodeId>
    {
        return graph::find_by_path(graph_, path, root);
    }

    [[nodiscard]]
    auto nodes_of_type(NodeType type) const -> std::vector<NodeId>
    {
        return graph::nodes_of_type(graph_, type);
    }

    [[nodiscard]]
    auto get_inputs(NodeId id) const -> std::vector<NodeId>
    {
        return graph::get_inputs(graph_, id);
    }

    [[nodiscard]]
    auto get_outputs(NodeId id) const -> std::vector<NodeId>
    {
        return graph::get_outputs(graph_, id);
    }

    [[nodiscard]]
    auto get_sticky_outputs(NodeId id) const -> std::vector<NodeId>
    {
        return graph::get_sticky_outputs(graph_, id);
    }

    [[nodiscard]]
    auto get_order_only(NodeId id) const -> std::vector<NodeId>
    {
        return graph::get_order_only(graph_, id);
    }

    [[nodiscard]]
    auto get_order_only_dependents(NodeId id) const -> std::vector<NodeId>
    {
        return graph::get_order_only_dependents(graph_, id);
    }

    [[nodiscard]]
    auto edges() const -> std::vector<Edge> const&
    {
        return graph_.edges;
    }

    [[nodiscard]]
    auto node_count() const -> std::size_t
    {
        return graph::node_count(graph_);
    }

    [[nodiscard]]
    auto edge_count() const -> std::size_t
    {
        return graph::edge_count(graph_);
    }

    [[nodiscard]]
    auto empty() const -> bool
    {
        return graph::empty(graph_);
    }

    auto clear() -> void { graph::clear(graph_); }

    [[nodiscard]]
    auto all_nodes() const -> std::vector<NodeId>
    {
        return graph::all_nodes(graph_);
    }

    [[nodiscard]]
    auto root_nodes() const -> std::vector<NodeId>
    {
        return graph::root_nodes(graph_);
    }

    [[nodiscard]]
    auto leaf_nodes() const -> std::vector<NodeId>
    {
        return graph::leaf_nodes(graph_);
    }

    [[nodiscard]]
    auto get_full_path(NodeId id) const -> std::string
    {
        return graph::get_full_path(graph_, id);
    }

    auto invalidate_path_cache(NodeId id) -> void { graph::invalidate_path_cache(graph_, id); }

    auto clear_path_cache() -> void { graph::clear_path_cache(graph_); }

    auto set_build_root_name(std::string name) -> void
    {
        graph::set_build_root_name(graph_, std::move(name));
    }

    [[nodiscard]]
    auto get_build_root_name() const -> std::string_view
    {
        return graph::get_build_root_name(graph_);
    }

    [[nodiscard]]
    auto is_under_build_root(NodeId id) const -> bool
    {
        return graph::is_under_build_root(graph_, id);
    }

    /// Access underlying graph for direct manipulation
    [[nodiscard]]
    auto graph() -> Graph&
    {
        return graph_;
    }

    [[nodiscard]]
    auto graph() const -> Graph const&
    {
        return graph_;
    }

private:
    Graph graph_;
};

} // namespace pup::graph
