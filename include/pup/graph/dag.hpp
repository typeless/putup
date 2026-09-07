// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/arena.hpp"
#include "pup/core/instruction.hpp"
#include "pup/core/node_id_map.hpp"
#include "pup/core/path_id.hpp"
#include "pup/core/path_pool.hpp"
#include "pup/core/result.hpp"
#include "pup/core/sorted_id_vec.hpp"
#include "pup/core/stable_vec.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/rule_pattern.hpp"

#include <cstdint>
#include <limits>
#include <optional>
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
};

/// File node - represents files, directories, groups, variables, ghosts
struct FileNode {
    NodeId id = 0;
    NodeType type = NodeType::File;
    NodeFlags flags = NodeFlags::None;

    StringId name = StringId::Empty; ///< Basename only (interned, tup-style identification)
    NodeId parent_dir = 0;           ///< Parent directory node (used with name for lookup)
    PathId path_id = PathId::Root;   ///< Structured path handle (reflect: NodeId → PathId)
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
/// instruction atoms + explicit operand NodeIds (inputs/outputs).
struct CommandNode {
    NodeId id = 0;

    StringId display = StringId::Empty;    ///< Display text (from ^ ^ markers, interned)
    StringId source_dir = StringId::Empty; ///< Tupfile directory (relative to root, interned)
    Instruction instruction = {};          ///< Instruction atoms (e.g. "gcc -c %f -o %o")

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
    StableVec<FileNode> files;           ///< Files, directories, groups (non-command nodes)
    StableVec<CommandNode> commands;     ///< Command nodes only
    StableVec<ConditionNode> conditions; ///< Condition nodes (for phi-node model)
    StableVec<PhiNode> phi_nodes;        ///< Phi nodes (merge conditional outputs)
    Vec<Edge> edges;                     ///< Central edge storage (single source of truth)

    Arena32 edge_arena;
    NodeIdArenaIndex edges_to_index;
    NodeIdArenaIndex edges_from_index;

    // Node lookup indices
    Vec<SortedPairVec> dir_children; ///< Per-directory name→NodeId index (indexed by parent dir)

    // Structured path algebra
    // mutable: memoized interning cache, written on the (const) read path.
    // Single-threaded access only — not safe to share across threads.
    mutable PathPool paths;     ///< Interning trie of (parent PathId, basename StringId) entries
    SortedPairVec path_to_node; ///< Resolve: PathId → NodeId (reverse of FileNode::path_id)

    NodeId next_file_id = 2;                               ///< Next file node ID (starts at 2, BUILD_ROOT is 1)
    NodeId next_command_id = node_id::make_command(1);     ///< Next command node ID
    NodeId next_condition_id = node_id::make_condition(1); ///< Next condition node ID
    NodeId next_phi_id = node_id::make_phi(1);             ///< Next phi node ID
};

/// Add a file node to the graph
[[nodiscard]]
auto add_file_node(Graph& graph, FileNode node) -> Result<NodeId>;

/// Create or find a file node from a PathId, walking the PathPool trie.
/// Creates intermediate directory nodes as needed.
/// Handles type upgrade (Ghost/File -> Generated).
[[nodiscard]]
auto ensure_file_node(Graph& graph, PathId path_id, NodeType type) -> Result<NodeId>;

/// Add a command node to the graph
[[nodiscard]]
auto add_command_node(Graph& graph, CommandNode node) -> Result<NodeId>;

/// Add an edge between nodes
[[nodiscard]]
auto add_edge(Graph& graph, NodeId from, NodeId to, LinkType type = LinkType::Normal) -> Result<void>;

/// Role tags for scalar-property access via get<Tag>. The four tags that
/// are also concrete types (NodeType, NodeFlags, Hash256, OutputAction)
/// identify themselves by their return type. The four below are pure role
/// tags whose storage type is StringId.
struct Name { };
struct Display { };
struct SourceDir { };
struct InstructionPattern { };

/// Storage type each get<Tag> maps to.
template<typename Tag>
struct get_storage;

template<>
struct get_storage<NodeType> {
    using type = NodeType;
};
template<>
struct get_storage<NodeFlags> {
    using type = NodeFlags;
};
template<>
struct get_storage<Hash256> {
    using type = Hash256;
};
template<>
struct get_storage<OutputAction> {
    using type = OutputAction;
};
template<>
struct get_storage<Name> {
    using type = StringId;
};
template<>
struct get_storage<Display> {
    using type = StringId;
};
template<>
struct get_storage<SourceDir> {
    using type = StringId;
};
template<>
struct get_storage<InstructionPattern> {
    using type = StringId;
};

/// Generic scalar-property accessor. Specialized per tag out-of-line in
/// dag.cpp. Callers that want a string_view for StringId-returning tags
/// can wrap the result in global_pool().get(...).
template<typename Tag>
[[nodiscard]]
auto get(Graph const& graph, NodeId id) -> typename get_storage<Tag>::type;

/// Role tags for composite-property access via view<Tag>.
struct Inputs { };
struct Outputs { };
struct ExportedVars { };

/// Storage type each view<Tag> maps to.
template<typename Tag>
struct view_storage;

template<>
struct view_storage<Inputs> {
    using type = Vec<NodeId>;
};
template<>
struct view_storage<Outputs> {
    using type = Vec<NodeId>;
};
template<>
struct view_storage<ExportedVars> {
    using type = SortedIdVec;
};

/// Composite-property accessor returning a range-like const reference.
/// Specialized per role tag out-of-line in dag.cpp. Use for collection
/// properties (inputs, outputs, exported vars) where iteration is the
/// natural access pattern.
template<typename Tag>
[[nodiscard]]
auto view(Graph const& graph, NodeId id) -> typename view_storage<Tag>::type const&;

/// Get the parent directory node of a file node
[[nodiscard]]
auto get_parent_dir(Graph const& graph, NodeId id) -> NodeId;

/// Get the parent command for InjectImplicitDeps commands
[[nodiscard]]
auto get_parent_command(Graph const& graph, NodeId id) -> NodeId;

/// Check if a command captures stdout (generated rule with Stdout output type)
[[nodiscard]]
auto is_stdout_capture(Graph const& graph, NodeId id) -> bool;

/// Check if all guards on a command are satisfied
[[nodiscard]]
auto is_guard_satisfied(Graph const& graph, NodeId id) -> bool;

/// Find a node by parent directory and basename (tup-style lookup)
[[nodiscard]]
auto find_by_dir_name(
    Graph const& graph,
    NodeId parent_dir,
    std::string_view name
) -> std::optional<NodeId>;

/// Get all nodes of a given type
[[nodiscard]]
auto nodes_of_type(Graph const& graph, NodeType type) -> Vec<NodeId>;

/// Edge traversal direction
enum class EdgeDirection : std::uint8_t {
    Forward,
    Backward,
};

/// Bitmask of LinkType values for edge filtering
using LinkTypeMask = std::uint8_t;

/// Create a LinkTypeMask from a single LinkType
[[nodiscard]]
constexpr auto link_type_bit(LinkType t) -> LinkTypeMask
{
    return LinkTypeMask { 1 } << (static_cast<std::uint8_t>(t) - 1);
}

/// Whether a link type belongs to a mask. Total over every byte an index can hold: edge
/// types are deserialized unvalidated, and a wild one must route nothing rather than shift
/// past the mask width and land on another type's bit.
[[nodiscard]]
constexpr auto in_mask(LinkType t, LinkTypeMask mask) -> bool
{
    return link_role(t) != LinkRole::Unknown && (link_type_bit(t) & mask) != 0;
}

/// Collect every link type whose role is one of those given. Sweeping the mask's whole
/// width rather than a list of enumerators keeps `link_role` the only place a new link
/// type has to be named; a bit naming no link type classifies as Unknown and joins nothing.
template<typename... Roles>
[[nodiscard]]
constexpr auto mask_of_roles(Roles... roles) -> LinkTypeMask
{
    auto mask = LinkTypeMask {};
    for (auto bit = std::uint8_t { 0 }; bit < std::numeric_limits<LinkTypeMask>::digits; ++bit) {
        auto type = static_cast<LinkType>(bit + 1);
        if (((link_role(type) == roles) || ...)) {
            mask |= link_type_bit(type);
        }
    }
    return mask;
}

/// The width dimension of what -Wswitch checks at `link_role`: a classified link type whose
/// value exceeds the mask's bits would join no mask and route nothing, silently.
constexpr auto every_link_type_fits_mask() -> bool
{
    for (auto value = std::numeric_limits<LinkTypeMask>::digits + 1; value <= 255; ++value) {
        if (link_role(static_cast<LinkType>(value)) != LinkRole::Unknown) {
            return false;
        }
    }
    return true;
}
static_assert(every_link_type_fits_mask(), "widen LinkTypeMask: a LinkType value exceeds its bit width");

namespace edge_mask {
inline constexpr auto data_flow = mask_of_roles(LinkRole::DataFlow);

/// Everything upstream of a command: what it reads, plus what decides its identity.
inline constexpr auto inputs = mask_of_roles(LinkRole::DataFlow, LinkRole::Identity);

inline constexpr auto sticky = mask_of_roles(LinkRole::Identity);

inline constexpr auto order_only = mask_of_roles(LinkRole::Ordering);

/// Every edge by which something consumes a node — data_flow omits order-only, but a file
/// reached only through `|` is still one whose absence is an error.
inline constexpr auto consumers = mask_of_roles(LinkRole::DataFlow, LinkRole::Ordering);

/// The half of `consumers` the live graph carries too, so routing it index-side is needed
/// only for a node that has left the graph.
inline constexpr auto parsed_consumers = static_cast<LinkTypeMask>(consumers & ~link_type_bit(LinkType::Implicit));

/// The half only the index carries: a discovered dependency is never parsed from a Tupfile,
/// so its consumer has no graph edge to cascade over and must be routed on every change.
inline constexpr auto discovered_consumers = link_type_bit(LinkType::Implicit);
} // namespace edge_mask

/// Visit neighbor ids by direction and type mask without materializing a Vec.
/// The allocation-free form for hot traversals (topo sort, reachability).
/// Deliberately a template (measured): an out-of-line fn-ptr core saved only
/// ~0.5 KB but cost most of the traversal speedup by blocking callback inlining.
template<typename Fn>
auto edges_for_each(Graph const& graph, NodeId id, EdgeDirection dir, LinkTypeMask mask, Fn&& fn) -> void
{
    auto const& index = (dir == EdgeDirection::Forward)
        ? graph.edges_from_index
        : graph.edges_to_index;

    auto s = index.get_slice(id);
    if (s.length == 0) {
        return;
    }
    for (auto idx : graph.edge_arena.slice(s)) {
        auto const& edge = graph.edges[idx];
        if (link_type_bit(edge.type) & mask) {
            fn(dir == EdgeDirection::Forward ? edge.to : edge.from);
        }
    }
}

/// Query edges by direction and type mask
[[nodiscard]]
auto edges_where(Graph const& graph, NodeId id, EdgeDirection dir, LinkTypeMask mask) -> Vec<NodeId>;

/// Get direct dependencies of a node
[[nodiscard]]
auto get_inputs(Graph const& graph, NodeId id) -> Vec<NodeId>;

/// Get direct dependents of a node (excludes sticky edges)
/// For build-time dependency traversal, use this function.
[[nodiscard]]
auto get_outputs(Graph const& graph, NodeId id) -> Vec<NodeId>;

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

/// Get all node IDs
[[nodiscard]]
auto all_nodes(Graph const& graph) -> Vec<NodeId>;

/// Get root nodes (nodes with no inputs)
[[nodiscard]]
auto root_nodes(Graph const& graph) -> Vec<NodeId>;

/// Materialize a PathId to its display string. For BuildRoot-grounded paths,
/// prepends the build root name (e.g., "build/gcc/foo.o").
[[nodiscard]]
auto materialize_path(Graph const& graph, PathId path_id) -> StringId;

/// Reconstruct full path from (parent_dir, name) chain
/// Uses provided cache for efficiency.
[[nodiscard]]
auto get_full_path(Graph const& graph, NodeId id, PathCache& cache) -> std::string_view;

/// Reconstruct full path without caching (convenience overload)
/// Note: Creates temporary cache - prefer the cached version for repeated calls.
[[nodiscard]]
auto get_full_path(Graph const& graph, NodeId id) -> StringId;

/// Clear the entire path cache
auto clear_path_cache(PathCache& cache) -> void;

/// Expand instruction pattern into full command string by substituting
/// operand paths (%f, %o, %b, %B, %e, %d, %O, %Nf, %No) from the graph.
[[nodiscard]]
auto expand_instruction(Graph const& graph, NodeId cmd_id, PathCache& cache) -> StringId;

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
) -> StringId;

/// Expand instruction pattern (convenience overload, creates temporary cache)
[[nodiscard]]
auto expand_instruction(Graph const& graph, NodeId cmd_id) -> StringId;

/// How to name a command in a message: its `^ ^` text if it has one, else what it will run.
/// Never empty — a rule without an annotation still has to be identifiable, and every caller
/// answering that for itself is how four of them came to print nothing at all (#229). The
/// overloads mirror expand_instruction's because the fallback is what it computes.
[[nodiscard]]
auto command_label(Graph const& graph, NodeId cmd_id, PathCache& cache) -> StringId;

[[nodiscard]]
auto command_label(
    Graph const& graph,
    NodeId cmd_id,
    PathCache& cache,
    std::string_view source_root,
    std::string_view config_root = {}
) -> StringId;

[[nodiscard]]
auto command_label(Graph const& graph, NodeId cmd_id) -> StringId;

/// What the command will do, for deciding whether it must re-run: the fully-expanded
/// command text plus the values (content hashes) of every Variable node it depends on
/// via a Sticky edge — capturing config/env vars that affect the output without
/// appearing in the rendered text (e.g. an exported env var read as $VAR).
///
/// Not a cross-build key. Editing a recipe changes the signature and must not make the
/// rule a different rule; that is what compute_command_key answers.
[[nodiscard]]
auto compute_command_signature(Graph const& graph, NodeId cmd_id, PathCache& cache) -> Hash256;

/// Which rule this is, for joining a command against the previous build. A command that
/// produces files is keyed by the files themselves — output ownership is unique among
/// guard-satisfied commands, enforced when the output edge is created — so this textual
/// key is the fallback for output-less commands, which have nothing else to be named by.
/// It deliberately excludes variable values: a var change means re-run, not a new rule.
[[nodiscard]]
auto compute_command_key(Graph const& graph, NodeId cmd_id, PathCache& cache) -> Hash256;

/// Set the build root name (relative path from source root to build root)
/// For in-tree builds, this should be empty. For variant builds, e.g. "build".
auto set_build_root_name(Graph& graph, std::string_view name) -> void;

/// Get the build root name
[[nodiscard]]
auto get_build_root_name(Graph const& graph) -> std::string_view;

// =============================================================================
// BuildGraph - thin data carrier (replaces BuildGraph over time)
// =============================================================================

/// Simple aggregate holding the graph and its path cache.
/// Replaces BuildGraph as a thin data carrier with no methods.
struct BuildGraph {
    Graph graph;
    // mutable: memoization written by get_full_path on the (const) read path.
    // Single-threaded access only — not safe to share across threads.
    mutable PathCache path_cache;
};

/// Create a BuildGraph with an initialized graph
[[nodiscard]]
auto make_build_graph() -> BuildGraph;

/// Collect all commands affected by the given changed files.
/// Uses forward traversal: starts at changed inputs, walks forward through outputs.
/// `ordering` supplies the successors no edge carries: a command that discovered a file must run
/// whenever the command producing that file does, and the graph has no edge saying so (#277).
[[nodiscard]]
auto collect_affected_commands(
    Graph const& graph,
    Vec<StringId> const& changed_files,
    Vec<NodeId> const& forced = {},
    Vec<OrderingEdge> const& ordering = {}
) -> NodeIdMap32;

/// Collect all commands required to build the given target nodes.
/// Uses reverse traversal: starts at targets, walks backward through inputs.
[[nodiscard]]
auto collect_required_commands(Graph const& graph, Vec<NodeId> const& target_ids) -> NodeIdMap32;

/// Collect commands in scope plus all transitive upstream producer commands.
/// Uses backward traversal from in-scope commands through inputs and order-only deps.
[[nodiscard]]
auto collect_scope_with_upstream_commands(
    Graph const& graph,
    Vec<StringId> const& scopes
) -> NodeIdMap32;

/// Collect all upstream input file paths for commands in the given scopes.
/// Returns sorted, deduplicated paths.
[[nodiscard]]
auto collect_upstream_files(
    BuildGraph const& state,
    Vec<StringId> const& scopes
) -> Vec<std::string_view>;

/// Set build root name and clear path cache
auto set_build_root_name(BuildGraph& state, std::string_view name) -> void;

} // namespace pup::graph
