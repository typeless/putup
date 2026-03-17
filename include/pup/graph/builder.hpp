// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "dag.hpp"
#include "pup/core/result.hpp"
#include "pup/core/sorted_id_vec.hpp"
#include "pup/core/string_id.hpp"
#include "pup/parser/ast.hpp"
#include "pup/parser/eval.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace pup::parser {
struct EvalContext;
}
namespace pup::parser {
class VarDb;
}

namespace pup::graph {

class DepScannerRegistry;
class RulePatternRegistry;

/// Options for graph building
struct BuilderOptions {
    std::string source_root;                                           ///< Source tree root (where source files live)
    std::string config_root;                                           ///< Config tree root (where Tupfiles live)
    std::string output_root;                                           ///< Output tree root (where outputs/.pup go)
    std::string config_path;                                           ///< Path to tup.config (for sticky edge tracking)
    bool expand_globs = true;                                          ///< Expand glob patterns
    bool validate_inputs = true;                                       ///< Check that input files exist
    bool verbose = false;                                              ///< Print verbose output
    DepScannerRegistry const* scanner_registry = nullptr;              ///< Optional scanner registry for implicit deps
    RulePatternRegistry const* pattern_registry = nullptr;             ///< Optional pattern registry for auto-generated rules
    std::vector<std::pair<std::string, std::string>> cached_env_vars = {}; ///< Cached env vars from previous build (sorted by key)
};

/// Bang macro definition
struct BangMacroDef {
    std::string name;
    bool foreach_ = false;
    std::vector<parser::PathPattern> order_only_inputs;
    parser::Expression command;
    std::optional<parser::Expression> display;
    std::vector<parser::PathPattern> outputs;
    std::vector<parser::PathPattern> extra_outputs;
    std::optional<std::string> output_group;                       ///< {binname} at end
    std::optional<std::string> output_order_only_group;            ///< <groupname> at end
    std::optional<parser::Expression> output_order_only_group_dir; ///< path/ prefix for <group>
};

/// Pending weak assignment with captured dependencies
struct PendingWeakAssignment {
    std::string name;
    std::string value;
    SortedIdVec config_deps; ///< Config var StringIds used in RHS
    SortedIdVec env_deps;    ///< Env var StringIds used in RHS
};

/// Context for building the graph (per-Tupfile state)
struct BuilderContext {
    BuildGraph* graph = nullptr;
    parser::EvalContext* eval = nullptr;
    parser::VarDb* vars = nullptr; ///< Variable database for import
    BuilderOptions options = {};

    std::vector<std::pair<std::uint32_t, BangMacroDef>> macros = {}; ///< Sorted by interned name key
    std::unordered_map<std::string, std::vector<NodeId>> groups = {};
    SortedIdVec included_files = {};
    SortedIdVec exported_vars = {}; ///< Interned environment variable names to export

    std::string current_dir = {};
    std::string current_file = {};
    std::vector<NodeId> sticky_sources = {}; ///< Tupfile + included files for sticky edges

    /// Config variable StringIds used during current command expansion (cleared per command)
    SortedIdVec used_config_vars = {};

    /// Env variable StringIds used during current command expansion (cleared per command)
    SortedIdVec used_env_vars = {};

    std::vector<std::string> errors = {};
    std::vector<std::string> warnings = {};

    /// Pending weak (??=) assignments - applied before rules, last wins
    std::vector<PendingWeakAssignment> pending_weak_assignments = {};

    /// Condition stack for phi-node model - tracks nested conditional guards
    /// Each entry is (condition_id, polarity). Commands created while in a conditional
    /// will have these guards applied.
    std::vector<Guard> condition_stack = {};

    /// Config variable StringIds used in enclosing conditions (for phi-node model).
    /// Commands inside conditionals need to depend on these vars to rebuild when
    /// the condition's value changes.
    SortedIdVec condition_config_vars = {};
};

// ============================================================================
// VarDepTracker - maps variable StringId to its set of dependency StringIds
// ============================================================================

struct VarDepTracker {
    SortedPairVec name_to_idx;     ///< StringId → index into pool
    std::vector<SortedIdVec> pool; ///< pool[idx] = dep set of StringIds

    [[nodiscard]]
    auto find(StringId key) const -> SortedIdVec const*
    {
        auto const* idx = name_to_idx.find(to_underlying(key));
        if (!idx) {
            return nullptr;
        }
        return &pool[*idx];
    }

    auto get_or_create(StringId key) -> SortedIdVec&
    {
        auto const* idx = name_to_idx.find(to_underlying(key));
        if (idx) {
            return pool[*idx];
        }
        auto new_idx = static_cast<std::uint32_t>(pool.size());
        pool.emplace_back();
        name_to_idx.insert(to_underlying(key), new_idx);
        return pool.back();
    }

    [[nodiscard]]
    auto empty() const -> bool
    {
        return name_to_idx.empty();
    }

    auto clear() -> void
    {
        name_to_idx.clear();
        pool.clear();
    }
};

// ============================================================================
// BuilderState - Persistent state across multiple Tupfiles
// ============================================================================

/// Deferred order-only edge reference for circular parsing situations
struct DeferredOrderOnlyEdge {
    NodeId group_id;
    NodeId command_id;

    auto operator<(DeferredOrderOnlyEdge const& other) const -> bool
    {
        return std::tie(group_id, command_id) < std::tie(other.group_id, other.command_id);
    }

    auto operator==(DeferredOrderOnlyEdge const& other) const -> bool = default;
};

/// Per-session state that persists across multiple Tupfiles
struct BuilderState {
    BuilderOptions options;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    /// Group node lookup: interned "directory/name" StringId → NodeId
    SortedPairVec group_nodes;

    /// Deferred edges to resolve after all Tupfiles are parsed
    std::vector<DeferredOrderOnlyEdge> deferred_edges;

    /// Config variable nodes (interned name StringId → NodeId)
    SortedPairVec config_var_nodes;

    /// Virtual $ directory for imported environment variables (like tup's env_dt)
    NodeId env_var_dir_id = INVALID_NODE_ID;

    /// Imported environment variable nodes (interned name StringId → NodeId)
    SortedPairVec imported_env_var_nodes;

    /// Set of imported variable name StringIds
    SortedIdVec imported_var_names;

    /// Track which regular variables depend on config vars (for transitive tracking)
    VarDepTracker var_config_deps;

    /// Track which regular variables depend on imported env vars (for transitive tracking)
    VarDepTracker var_env_deps;
};

// ============================================================================
// Free function API
// ============================================================================

/// Create a new builder state with the given options
[[nodiscard]]
auto make_builder_state(BuilderOptions opts) -> BuilderState;

/// Build graph from a single Tupfile AST
[[nodiscard]]
auto build_graph(
    parser::Tupfile const& tupfile,
    parser::EvalContext& eval,
    BuilderState& state
) -> Result<BuildGraph>;

/// Add a Tupfile to an existing graph
[[nodiscard]]
auto add_tupfile(
    BuildGraph& graph,
    parser::Tupfile const& tupfile,
    parser::EvalContext& eval,
    BuilderState& state
) -> Result<void>;

/// Resolve deferred order-only edges after all Tupfiles are parsed
[[nodiscard]]
auto resolve_deferred_order_only_edges(
    BuildGraph& graph,
    BuilderState& state
) -> Result<void>;

// ============================================================================
// GraphBuilder - Thin wrapper for backward compatibility
// ============================================================================

/// Build graph from a parsed Tupfile (thin wrapper around free functions)
class GraphBuilder {
public:
    explicit GraphBuilder(BuilderOptions options = {});
    ~GraphBuilder() = default;

    GraphBuilder(GraphBuilder const&) = delete;
    auto operator=(GraphBuilder const&) -> GraphBuilder& = delete;

    GraphBuilder(GraphBuilder&&) noexcept = default;
    auto operator=(GraphBuilder&&) noexcept -> GraphBuilder& = default;

    /// Build graph from a single Tupfile AST
    [[nodiscard]]
    auto build(
        parser::Tupfile const& tupfile,
        parser::EvalContext& eval
    ) -> Result<BuildGraph>;

    /// Add a Tupfile to an existing graph
    [[nodiscard]]
    auto add_tupfile(
        BuildGraph& graph,
        parser::Tupfile const& tupfile,
        parser::EvalContext& eval
    ) -> Result<void>;

    /// Get build errors
    [[nodiscard]]
    auto errors() const -> std::vector<std::string> const&;

    /// Get build warnings
    [[nodiscard]]
    auto warnings() const -> std::vector<std::string> const&;

    /// Resolve deferred order-only edges after all Tupfiles are parsed
    [[nodiscard]]
    auto resolve_deferred_order_only_edges(BuildGraph& graph) -> Result<void>;

    /// Access underlying state for direct manipulation
    [[nodiscard]]
    auto state() -> BuilderState&;
    [[nodiscard]]
    auto state() const -> BuilderState const&;

private:
    BuilderState state_;
};

} // namespace pup::graph
