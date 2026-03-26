// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/arena.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/node_id_map.hpp"
#include "pup/core/paged_vec.hpp"
#include "pup/core/result.hpp"
#include "pup/core/sorted_id_vec.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/rule_pattern.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace pup::graph {

/// Path cache - maps NodeId to interned full path string
struct PathCache {
    NodeIdMap32 ids; ///< NodeId → StringId (path interned in pool)
    StringPool pool; ///< Owns the full path strings
};

/// Edge between nodes in the build graph
struct Edge {
    NodeId from = 0;
    NodeId to = 0;
    LinkType type = LinkType::Normal;
    NodeId group_cmd_id = 0; ///< For group edges, the command that produced the group
};

/// File node - represents files, directories, groups, variables, ghosts
struct FileNode {
    NodeId id = 0;
    NodeType type = NodeType::File;
    NodeFlags flags = NodeFlags::None;

    StringId name = StringId::Empty; ///< Basename only (interned, tup-style identification)
    NodeId parent_dir = 0;           ///< Parent directory node (used with name for lookup)
    Hash256 content_hash = { {} };   ///< Content hash (double braces force zero-init)
};

/// Guard entry - a condition and the polarity (true/false) required for this guard
struct Guard {
    NodeId condition = INVALID_NODE_ID; ///< Condition node ID
    bool polarity = true;               ///< True if condition must be true, false if must be false

    auto operator==(Guard const& other) const -> bool = default;
};

/// Command node - represents build commands
/// Note: Type is determined by node_id::is_command(), not a stored field.
/// Command string is reconstructed on demand via expand_instruction() from
/// instruction_id + explicit operand NodeIds (inputs/outputs).
struct CommandNode {
    NodeId id = 0;

    StringId display = StringId::Empty;        ///< Display text (from ^ ^ markers, interned)
    StringId source_dir = StringId::Empty;     ///< Tupfile directory (relative to root, interned)
    StringId instruction_id = StringId::Empty; ///< Instruction pattern (e.g. "gcc -c %f -o %o")

    Vec<NodeId> inputs = {};  ///< Operand file NodeIds for %f expansion
    Vec<NodeId> outputs = {}; ///< Operand file NodeIds for %o expansion

    SortedIdVec exported_vars = {}; ///< Env vars to export to command (interned StringIds)

    // For generated rules (auto-generated from pattern matching)
    std::optional<GeneratedOutput> generated_output = {}; ///< Output specification
    OutputAction output_action = {};                      ///< What to do with output
    NodeId parent_command = INVALID_NODE_ID;              ///< Parent command for InjectImplicitDeps

    // Condition guards - command executes only if ALL guards are satisfied
    // For nested conditionals, this accumulates all enclosing conditions
    Vec<Guard> guards = {};
};

/// Condition node - represents an ifeq/ifdef/ifneq/ifndef condition
/// Used for phi-node model: both branches are in graph, guards determine which executes
struct ConditionNode {
    NodeId id = 0;
    StringId expression = StringId::Empty;   ///< String representation of condition
    bool current_value = false;              ///< Evaluated at graph time
    NodeId config_var_dep = INVALID_NODE_ID; ///< Config var this depends on (if any)
};

/// Phi node - merges outputs from conditional branches
/// When multiple commands produce the same output from different branches,
/// the phi node resolves to the active branch's output
struct PhiNode {
    NodeId id = 0;
    StringId name = StringId::Empty;      ///< Canonical output name
    NodeId parent_dir = 0;                ///< Parent directory
    NodeId condition = INVALID_NODE_ID;   ///< The condition determining which output to use
    NodeId then_output = INVALID_NODE_ID; ///< Output when condition is true
    NodeId else_output = INVALID_NODE_ID; ///< Output when condition is false
};

/// Build graph - DAG of nodes and edges (plain data struct)
struct Graph {
    PagedVec<FileNode> files;           ///< Files, directories, groups (non-command nodes)
    PagedVec<CommandNode> commands;     ///< Command nodes only
    PagedVec<ConditionNode> conditions; ///< Condition nodes (for phi-node model)
    PagedVec<PhiNode> phi_nodes;        ///< Phi nodes (merge conditional outputs)
    Vec<Edge> edges;                    ///< Central edge storage (single source of truth)

    Arena32 edge_arena;
    NodeIdArenaIndex edges_to_index;
    NodeIdArenaIndex edges_from_index;
    NodeIdArenaIndex order_only_to_index;
    NodeIdArenaIndex order_only_dependents;

    // Node lookup indices
    Vec<SortedPairVec> dir_children; ///< Per-directory name→NodeId index (indexed by parent dir)
    StringPool command_strings;      ///< Interned expanded command strings (separate pool for find_by_command)
    SortedPairVec command_index;     ///< StringId(command) → NodeId
    bool command_index_built = false;

    NodeId next_file_id = 2;                               ///< Next file node ID (starts at 2, BUILD_ROOT is 1)
    NodeId next_command_id = node_id::make_command(1);     ///< Next command node ID
    NodeId next_condition_id = node_id::make_condition(1); ///< Next condition node ID
    NodeId next_phi_id = node_id::make_phi(1);             ///< Next phi node ID
};

/// Create a new empty graph with build root initialized
[[nodiscard]]
auto make_graph() -> Graph;

/// Validate that a node ID exists in the graph
[[nodiscard]]
auto validate_node_id(Graph const& graph, NodeId id) -> bool;

/// Add a file node to the graph
[[nodiscard]]
auto add_file_node(Graph& graph, FileNode node) -> Result<NodeId>;

/// Add a command node to the graph
[[nodiscard]]
auto add_command_node(Graph& graph, CommandNode node) -> Result<NodeId>;

/// Add an edge between nodes
[[nodiscard]]
auto add_edge(Graph& graph, NodeId from, NodeId to, LinkType type = LinkType::Normal) -> Result<void>;

/// Add an order-only edge (dependency that doesn't trigger rebuild)
[[nodiscard]]
auto add_order_only_edge(Graph& graph, NodeId from, NodeId to) -> Result<void>;

/// Get a file node by ID (mutable) - returns nullptr for command IDs
[[nodiscard]]
auto get_file_node(Graph& graph, NodeId id) -> FileNode*;

/// Get a file node by ID (const) - returns nullptr for command IDs
[[nodiscard]]
auto get_file_node(Graph const& graph, NodeId id) -> FileNode const*;

/// Get a command node by ID (mutable) - returns nullptr for file IDs
[[nodiscard]]
auto get_command_node(Graph& graph, NodeId id) -> CommandNode*;

/// Get a command node by ID (const) - returns nullptr for file IDs
[[nodiscard]]
auto get_command_node(Graph const& graph, NodeId id) -> CommandNode const*;

/// Add a condition node to the graph
[[nodiscard]]
auto add_condition_node(Graph& graph, ConditionNode node) -> Result<NodeId>;

/// Get a condition node by ID (mutable) - returns nullptr for non-condition IDs
[[nodiscard]]
auto get_condition_node(Graph& graph, NodeId id) -> ConditionNode*;

/// Get a condition node by ID (const) - returns nullptr for non-condition IDs
[[nodiscard]]
auto get_condition_node(Graph const& graph, NodeId id) -> ConditionNode const*;

/// Add a phi node to the graph
[[nodiscard]]
auto add_phi_node(Graph& graph, PhiNode node) -> Result<NodeId>;

/// Get a phi node by ID (mutable) - returns nullptr for non-phi IDs
[[nodiscard]]
auto get_phi_node(Graph& graph, NodeId id) -> PhiNode*;

/// Get a phi node by ID (const) - returns nullptr for non-phi IDs
[[nodiscard]]
auto get_phi_node(Graph const& graph, NodeId id) -> PhiNode const*;

/// Resolve a phi node to its active output based on current condition values
[[nodiscard]]
auto resolve_phi_node(Graph const& graph, NodeId phi_id) -> NodeId;

/// Check if all guards on a command are satisfied
[[nodiscard]]
auto is_guard_satisfied(Graph const& graph, CommandNode const& cmd) -> bool;

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
auto nodes_of_type(Graph const& graph, NodeType type) -> Vec<NodeId>;

/// Get direct dependencies of a node
[[nodiscard]]
auto get_inputs(Graph const& graph, NodeId id) -> Vec<NodeId>;

/// Get direct dependents of a node (excludes sticky edges)
/// For build-time dependency traversal, use this function.
[[nodiscard]]
auto get_outputs(Graph const& graph, NodeId id) -> Vec<NodeId>;

/// Get sticky dependents of a node (Tupfile/config dependencies)
/// Sticky edges are parse-time dependencies - use for reparse decisions, not rebuilds.
[[nodiscard]]
auto get_sticky_outputs(Graph const& graph, NodeId id) -> Vec<NodeId>;

/// Get order-only dependencies
[[nodiscard]]
auto get_order_only(Graph const& graph, NodeId id) -> Vec<NodeId>;

/// Get nodes that have this node as an order-only dependency
[[nodiscard]]
auto get_order_only_dependents(Graph const& graph, NodeId id) -> Vec<NodeId>;

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
auto all_nodes(Graph const& graph) -> Vec<NodeId>;

/// Get root nodes (nodes with no inputs)
[[nodiscard]]
auto root_nodes(Graph const& graph) -> Vec<NodeId>;

/// Get leaf nodes (nodes with no outputs)
[[nodiscard]]
auto leaf_nodes(Graph const& graph) -> Vec<NodeId>;

/// Reconstruct full path from (parent_dir, name) chain
/// Uses provided cache for efficiency.
[[nodiscard]]
auto get_full_path(Graph const& graph, NodeId id, PathCache& cache) -> std::string_view;

/// Reconstruct full path without caching (convenience overload)
/// Note: Creates temporary cache - prefer the cached version for repeated calls.
[[nodiscard]]
auto get_full_path(Graph const& graph, NodeId id) -> String;

/// Invalidate path cache entry for a node (call when parent_dir or name changes)
auto invalidate_path_cache(PathCache& cache, NodeId id) -> void;

/// Clear the entire path cache
auto clear_path_cache(PathCache& cache) -> void;

/// Get file node name as string_view
[[nodiscard]]
auto get_name(Graph const& graph, NodeId id) -> std::string_view;

/// Expand instruction pattern into full command string by substituting
/// operand paths (%f, %o, %b, %B, %e, %d, %O, %Nf, %No) from the graph.
[[nodiscard]]
auto expand_instruction(Graph const& graph, NodeId cmd_id, PathCache& cache) -> String;

/// Expand instruction with canonical path resolution for symlinked source trees.
/// When source_root is provided, build-tree paths are computed relative to the
/// canonical (physical) CWD instead of the logical CWD.
/// In 3-tree builds, config_root locates overlay files (like defaults.config)
/// that live alongside Tupfiles rather than in source_root.
[[nodiscard]]
auto expand_instruction(
    Graph const& graph,
    NodeId cmd_id,
    PathCache& cache,
    std::string_view source_root,
    std::string_view config_root = {}
) -> String;

/// Expand instruction pattern (convenience overload, creates temporary cache)
[[nodiscard]]
auto expand_instruction(Graph const& graph, NodeId cmd_id) -> String;

/// Build the command string index for find_by_command() lookups.
/// Must be called after all commands have their operands set (post-parsing).
auto build_command_index(Graph& graph, PathCache& cache) -> void;

/// Get command node display string as string_view
[[nodiscard]]
auto get_display_str(Graph const& graph, NodeId id) -> std::string_view;

/// Get command node source directory as string_view
[[nodiscard]]
auto get_source_dir(Graph const& graph, NodeId id) -> std::string_view;

/// Get command node instruction pattern as string_view (pre-pattern-expansion)
[[nodiscard]]
auto get_instruction_pattern(Graph const& graph, NodeId id) -> std::string_view;

/// Set the build root name (relative path from source root to build root)
/// For in-tree builds, this should be empty. For variant builds, e.g. "build".
auto set_build_root_name(Graph& graph, std::string_view name) -> void;

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
    auto add_file_node(FileNode node) -> Result<NodeId>
    {
        return graph::add_file_node(graph_, std::move(node));
    }

    [[nodiscard]]
    auto add_command_node(CommandNode node) -> Result<NodeId>
    {
        return graph::add_command_node(graph_, std::move(node));
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
    auto get_file_node(NodeId id) -> FileNode*
    {
        return graph::get_file_node(graph_, id);
    }

    [[nodiscard]]
    auto get_file_node(NodeId id) const -> FileNode const*
    {
        return graph::get_file_node(graph_, id);
    }

    [[nodiscard]]
    auto get_command_node(NodeId id) -> CommandNode*
    {
        return graph::get_command_node(graph_, id);
    }

    [[nodiscard]]
    auto get_command_node(NodeId id) const -> CommandNode const*
    {
        return graph::get_command_node(graph_, id);
    }

    [[nodiscard]]
    auto add_condition_node(ConditionNode node) -> Result<NodeId>
    {
        return graph::add_condition_node(graph_, std::move(node));
    }

    [[nodiscard]]
    auto get_condition_node(NodeId id) -> ConditionNode*
    {
        return graph::get_condition_node(graph_, id);
    }

    [[nodiscard]]
    auto get_condition_node(NodeId id) const -> ConditionNode const*
    {
        return graph::get_condition_node(graph_, id);
    }

    [[nodiscard]]
    auto add_phi_node(PhiNode node) -> Result<NodeId>
    {
        return graph::add_phi_node(graph_, std::move(node));
    }

    [[nodiscard]]
    auto get_phi_node(NodeId id) -> PhiNode*
    {
        return graph::get_phi_node(graph_, id);
    }

    [[nodiscard]]
    auto get_phi_node(NodeId id) const -> PhiNode const*
    {
        return graph::get_phi_node(graph_, id);
    }

    [[nodiscard]]
    auto resolve_phi_node(NodeId phi_id) const -> NodeId
    {
        return graph::resolve_phi_node(graph_, phi_id);
    }

    [[nodiscard]]
    auto is_guard_satisfied(CommandNode const& cmd) const -> bool
    {
        return graph::is_guard_satisfied(graph_, cmd);
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
    auto nodes_of_type(NodeType type) const -> Vec<NodeId>
    {
        return graph::nodes_of_type(graph_, type);
    }

    [[nodiscard]]
    auto get_inputs(NodeId id) const -> Vec<NodeId>
    {
        return graph::get_inputs(graph_, id);
    }

    [[nodiscard]]
    auto get_outputs(NodeId id) const -> Vec<NodeId>
    {
        return graph::get_outputs(graph_, id);
    }

    [[nodiscard]]
    auto get_sticky_outputs(NodeId id) const -> Vec<NodeId>
    {
        return graph::get_sticky_outputs(graph_, id);
    }

    [[nodiscard]]
    auto get_order_only(NodeId id) const -> Vec<NodeId>
    {
        return graph::get_order_only(graph_, id);
    }

    [[nodiscard]]
    auto get_order_only_dependents(NodeId id) const -> Vec<NodeId>
    {
        return graph::get_order_only_dependents(graph_, id);
    }

    [[nodiscard]]
    auto edges() const -> Vec<Edge> const&
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
    auto all_nodes() const -> Vec<NodeId>
    {
        return graph::all_nodes(graph_);
    }

    [[nodiscard]]
    auto root_nodes() const -> Vec<NodeId>
    {
        return graph::root_nodes(graph_);
    }

    [[nodiscard]]
    auto leaf_nodes() const -> Vec<NodeId>
    {
        return graph::leaf_nodes(graph_);
    }

    [[nodiscard]]
    auto get_full_path(NodeId id) const -> String
    {
        return String { graph::get_full_path(graph_, id, path_cache_) };
    }

    [[nodiscard]]
    auto expand_instruction(NodeId id) const -> String
    {
        return graph::expand_instruction(graph_, id, path_cache_);
    }

    auto build_command_index() -> void
    {
        graph::build_command_index(graph_, path_cache_);
    }

    auto invalidate_path_cache(NodeId id) -> void { graph::invalidate_path_cache(path_cache_, id); }

    auto clear_path_cache() -> void { graph::clear_path_cache(path_cache_); }

    /// Get direct access to the path cache (for advanced use)
    [[nodiscard]]
    auto path_cache() -> PathCache&
    {
        return path_cache_;
    }

    [[nodiscard]]
    auto path_cache() const -> PathCache const&
    {
        return path_cache_;
    }

    auto set_build_root_name(std::string_view name) -> void
    {
        graph::set_build_root_name(graph_, name);
        graph::clear_path_cache(path_cache_);
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

    /// Intern a string in the global string pool
    [[nodiscard]]
    auto intern(std::string_view str) -> StringId
    {
        return global_pool().intern(str);
    }

    /// Get string from the global string pool
    [[nodiscard]]
    auto str(StringId id) const -> std::string_view
    {
        return global_pool().get(id);
    }

    [[nodiscard]]
    auto string_pool() -> StringPool&
    {
        return global_pool();
    }

    [[nodiscard]]
    auto string_pool() const -> StringPool const&
    {
        return global_pool();
    }

private:
    Graph graph_;
    mutable PathCache path_cache_; ///< Path cache owned by BuildGraph (not Graph)
};

} // namespace pup::graph
