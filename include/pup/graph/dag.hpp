// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "pup/core/result.hpp"
#include "pup/core/types.hpp"
#include "pup/graph/rule_pattern.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pup::graph {

/// Hash for (parent_dir, name) lookup key
struct DirNameKey {
    NodeId parent_dir = 0;
    std::string name = {};

    auto operator==(DirNameKey const& other) const -> bool = default;
};

struct DirNameKeyHash {
    auto operator()(DirNameKey const& key) const noexcept -> std::size_t
    {
        auto h1 = std::hash<NodeId> {}(key.parent_dir);
        auto h2 = std::hash<std::string> {}(key.name);
        // Use boost::hash_combine pattern for better distribution
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

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

    std::string path = {};       ///< For files: relative path from tup root (deprecated, use name)
    std::string name = {};       ///< Basename only (new: tup-style identification)
    std::string command = {};    ///< For commands: the command string
    std::string display = {};    ///< For commands: display text (from ^ ^ markers)
    std::string source_dir = {}; ///< For commands: Tupfile directory (relative to root)

    NodeId parent_dir = 0;     ///< Parent directory node (used with name for lookup)
    Hash256 content_hash = {}; ///< Content hash for files
    FileTime mtime = {};       ///< Modification time

    std::vector<NodeId> inputs = {};          ///< Input edges (dependencies)
    std::vector<NodeId> outputs = {};         ///< Output edges (dependents)
    std::vector<NodeId> order_only = {};      ///< Order-only dependencies
    std::set<std::string> exported_vars = {}; ///< Env vars to export to command

    // For generated rules (auto-generated from pattern matching)
    std::optional<GeneratedOutput> generated_output = {}; ///< Output specification
    OutputAction output_action = {};                      ///< What to do with output
    NodeId parent_command = INVALID_NODE_ID;              ///< Parent command for InjectImplicitDeps
};

/// Build graph - DAG of nodes and edges
class BuildGraph {
public:
    BuildGraph() = default;

    /// Add a node to the graph
    [[nodiscard]] auto add_node(Node node) -> Result<NodeId>;

    /// Add an edge between nodes
    [[nodiscard]] auto add_edge(NodeId from, NodeId to, LinkType type = LinkType::Normal)
        -> Result<void>;

    /// Add an order-only edge (dependency that doesn't trigger rebuild)
    [[nodiscard]] auto add_order_only_edge(NodeId from, NodeId to) -> Result<void>;

    /// Get a node by ID
    [[nodiscard]] auto get_node(NodeId id) -> Node*;
    [[nodiscard]] auto get_node(NodeId id) const -> Node const*;

    /// Get mutable node by ID (alias for non-const get_node)
    [[nodiscard]] auto get_node_mut(NodeId id) -> Node* { return get_node(id); }

    /// Find a node by path (deprecated, use find_by_dir_name)
    [[nodiscard]] auto find_by_path(std::string_view path) const -> std::optional<NodeId>;

    /// Find a node by parent directory and basename (tup-style lookup)
    [[nodiscard]] auto find_by_dir_name(NodeId parent_dir, std::string_view name) const
        -> std::optional<NodeId>;

    /// Find a node by command string
    [[nodiscard]] auto find_by_command(std::string_view cmd) const -> std::optional<NodeId>;

    /// Get all nodes of a given type
    [[nodiscard]] auto nodes_of_type(NodeType type) const -> std::vector<NodeId>;

    /// Get direct dependencies of a node
    [[nodiscard]] auto get_inputs(NodeId id) const -> std::vector<NodeId>;

    /// Get direct dependents of a node
    [[nodiscard]] auto get_outputs(NodeId id) const -> std::vector<NodeId>;

    /// Get order-only dependencies
    [[nodiscard]] auto get_order_only(NodeId id) const -> std::vector<NodeId>;

    /// Get nodes that have this node as an order-only dependency
    [[nodiscard]] auto get_order_only_dependents(NodeId id) const -> std::vector<NodeId>;

    /// Get all edges
    [[nodiscard]] auto edges() const -> std::vector<Edge> const& { return edges_; }

    /// Get total number of nodes
    [[nodiscard]] auto node_count() const -> std::size_t { return nodes_.empty() ? 0 : nodes_.size() - 1; }

    /// Get total number of edges
    [[nodiscard]] auto edge_count() const -> std::size_t { return edges_.size(); }

    /// Check if graph is empty
    [[nodiscard]] auto empty() const -> bool { return nodes_.empty(); }

    /// Clear the graph
    auto clear() -> void;

    /// Get all node IDs
    [[nodiscard]] auto all_nodes() const -> std::vector<NodeId>;

    /// Get root nodes (nodes with no inputs)
    [[nodiscard]] auto root_nodes() const -> std::vector<NodeId>;

    /// Get leaf nodes (nodes with no outputs)
    [[nodiscard]] auto leaf_nodes() const -> std::vector<NodeId>;

    /// Reconstruct full path from (parent_dir, name) chain
    /// Uses internal cache for efficiency. Returns node->path if name is empty (legacy nodes).
    [[nodiscard]] auto get_full_path(NodeId id) const -> std::string;

    /// Invalidate path cache for a node (call when parent_dir or name changes)
    auto invalidate_path_cache(NodeId id) -> void;

    /// Clear the entire path cache
    auto clear_path_cache() -> void;

    /// Iterator support
    [[nodiscard]] auto begin() { return nodes_.begin(); }
    [[nodiscard]] auto end() { return nodes_.end(); }
    [[nodiscard]] auto begin() const { return nodes_.begin(); }
    [[nodiscard]] auto end() const { return nodes_.end(); }

private:
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::unordered_map<std::string, NodeId> path_index_;
    std::unordered_map<DirNameKey, NodeId, DirNameKeyHash> dir_name_index_;
    std::unordered_map<std::string, NodeId> command_index_;
    std::unordered_map<NodeId, std::vector<NodeId>> order_only_dependents_;
    mutable std::unordered_map<NodeId, std::string> path_cache_;
    NodeId next_id_ = 1;

    [[nodiscard]] auto validate_node_id(NodeId id) const -> bool;
};

} // namespace pup::graph
