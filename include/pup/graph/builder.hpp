// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "dag.hpp"
#include "pup/core/result.hpp"
#include "pup/core/sorted_id_vec.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"
#include "pup/parser/ast.hpp"
#include "pup/parser/eval.hpp"

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
    StringId source_root = StringId::Empty;                  ///< Source tree root (where source files live)
    StringId config_root = StringId::Empty;                  ///< Config tree root (where Tupfiles live)
    StringId output_root = StringId::Empty;                  ///< Output tree root (where outputs/.pup go)
    StringId config_path = StringId::Empty;                  ///< Path to tup.config (for sticky edge tracking)
    bool expand_globs = true;                                ///< Expand glob patterns
    bool validate_inputs = true;                             ///< Check that input files exist
    bool verbose = false;                                    ///< Print verbose output
    DepScannerRegistry const* scanner_registry = nullptr;    ///< Optional scanner registry for implicit deps
    RulePatternRegistry const* pattern_registry = nullptr;   ///< Optional pattern registry for auto-generated rules
    Vec<std::pair<StringId, StringId>> cached_env_vars = {}; ///< Cached env vars from previous build (sorted by key)
};

/// Bang macro definition
struct BangMacroDef {
    StringId name = StringId::Empty;
    bool foreach_ = false;
    Vec<parser::PathPattern> order_only_inputs;
    parser::Expression command;
    std::optional<parser::Expression> display;
    Vec<parser::PathPattern> outputs;
    Vec<parser::PathPattern> extra_outputs;
    std::optional<StringId> output_group;                          ///< {binname} at end
    std::optional<StringId> output_order_only_group;               ///< <groupname> at end
    std::optional<parser::Expression> output_order_only_group_dir; ///< path/ prefix for <group>
};

/// Pending weak assignment with captured dependencies
struct PendingWeakAssignment {
    StringId name = StringId::Empty;
    StringId value = StringId::Empty;
    SortedIdVec config_deps; ///< Config var StringIds used in RHS
    SortedIdVec env_deps;    ///< Env var StringIds used in RHS
};

// ============================================================================
// GroupMemberTable - maps interned group name to its list of member NodeIds
// ============================================================================

struct GroupMemberTable {
    SortedPairVec name_to_idx; ///< Interned group name → index into pool
    Vec<Vec<NodeId>> pool;     ///< pool[idx] = member NodeIds

    [[nodiscard]]
    auto find(std::uint32_t key) const -> Vec<NodeId> const*
    {
        auto const* idx = name_to_idx.find(key);
        if (!idx) {
            return nullptr;
        }
        return &pool[*idx];
    }

    auto get_or_create(std::uint32_t key) -> Vec<NodeId>&
    {
        auto const* idx = name_to_idx.find(key);
        if (idx) {
            return pool[*idx];
        }
        auto new_idx = static_cast<std::uint32_t>(pool.size());
        pool.emplace_back();
        name_to_idx.insert(key, new_idx);
        return pool.back();
    }
};

/// Context for building the graph (per-Tupfile state)
struct BuilderContext {
    BuildGraph* state = nullptr;
    parser::EvalContext* eval = nullptr;
    parser::VarDb* vars = nullptr; ///< Variable database for import
    BuilderOptions options = {};

    Vec<std::pair<std::uint32_t, BangMacroDef>> macros = {}; ///< Sorted by interned name key
    GroupMemberTable groups = {};
    SortedIdVec included_contexts = {}; ///< Interned (path, guard stack) keys
    SortedIdVec exported_vars = {};     ///< Interned environment variable names to export

    StringId current_dir = StringId::Empty;
    StringId current_file = StringId::Empty;
    Vec<NodeId> sticky_sources = {}; ///< Tupfile + included files for sticky edges

    /// Config variable StringIds used during current command expansion (cleared per command)
    SortedIdVec used_config_vars = {};

    /// Env variable StringIds used during current command expansion (cleared per command)
    SortedIdVec used_env_vars = {};

    Vec<StringId> errors = {};
    Vec<StringId> warnings = {};

    /// Pending weak (??=) assignments - applied before rules, last wins
    Vec<PendingWeakAssignment> pending_weak_assignments = {};

    /// Condition stack for phi-node model - tracks nested conditional guards
    /// Each entry is (condition_id, polarity). Commands created while in a conditional
    /// will have these guards applied.
    Vec<Guard> condition_stack = {};

    /// Config variable StringIds used in enclosing conditions (for phi-node model).
    /// Commands inside conditionals need to depend on these vars to rebuild when
    /// the condition's value changes.
    SortedIdVec condition_config_vars = {};

    /// Env variable StringIds used in enclosing conditions (symmetric counterpart of
    /// condition_config_vars). Without this, a guard flip driven by an env-sourced
    /// $(TUP_PLATFORM)-style var would not change any command's identity and the
    /// rebuild would be missed.
    SortedIdVec condition_env_vars = {};
};

// ============================================================================
// VarDepTracker - maps variable StringId to its set of dependency StringIds
// ============================================================================

struct VarDepTracker {
    SortedPairVec name_to_idx; ///< StringId → index into pool
    Vec<SortedIdVec> pool;     ///< pool[idx] = dep set of StringIds

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
// Builder - Persistent state across multiple Tupfiles
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
struct Builder {
    BuilderOptions options;
    Vec<StringId> errors;
    Vec<StringId> warnings;

    /// Group node lookup: interned "directory/name" StringId → NodeId
    SortedPairVec group_nodes;

    /// Deferred edges to resolve after all Tupfiles are parsed
    Vec<DeferredOrderOnlyEdge> deferred_edges;

    /// Config variable nodes (interned name StringId → NodeId)
    SortedPairVec config_var_nodes;

    /// Virtual $ directory for imported environment variables (like tup's env_dt)
    NodeId env_var_dir_id = INVALID_NODE_ID;

    /// Synthetic Variable node holding the CONFIG_TRACKED_TOOLS fingerprint
    NodeId toolchain_node_id = INVALID_NODE_ID;

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
auto make_builder(BuilderOptions opts) -> Builder;

/// Add a Tupfile to an existing build state
[[nodiscard]]
auto add_tupfile(
    BuildGraph& build_state,
    parser::Tupfile const& tupfile,
    parser::EvalContext& eval,
    Builder& state
) -> Result<void>;

/// Finalize the graph after all Tupfiles are parsed.
/// Resolves deferred group edges and expands %<group> patterns in commands.
[[nodiscard]]
auto finalize_graph(
    BuildGraph& build_state,
    Builder& state
) -> Result<void>;

} // namespace pup::graph
