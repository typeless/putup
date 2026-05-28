// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/builder.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/expected.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/hash.hpp"
#include "pup/core/node_id_map.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/core/result.hpp"
#include "pup/core/sorted_id_vec.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/rule_pattern.hpp"
#include "pup/parser/ast.hpp"
#include "pup/parser/eval.hpp"
#include "pup/parser/glob.hpp"
#include "pup/parser/parser.hpp"

#include "pup/core/path.hpp"
#include "pup/platform/file_io.hpp"

#include <cstdint>
#include <cstdio>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace pup::graph {

// Graph-module-internal functions (defined in dag.cpp, not in public header)
auto get_file_node(Graph& graph, NodeId id) -> FileNode*;
auto get_command_node(Graph& graph, NodeId id) -> CommandNode*;
auto add_condition_node(Graph& graph, ConditionNode node) -> Result<NodeId>;
auto get_condition_node(Graph const& graph, NodeId id) -> ConditionNode const*;

namespace {
// ---------------------------------------------------------------------------
// §1 — Inline helpers
// ---------------------------------------------------------------------------

auto str(StringId id) -> std::string_view { return global_pool().get(id); }
auto intern(std::string_view s) -> StringId { return global_pool().intern(s); }

/// RAII scope guard for cleanup on scope exit (zero-overhead via template)
template<typename F>
struct ScopeGuard {
    F cleanup;
    explicit ScopeGuard(F fn)
        : cleanup { std::move(fn) }
    {
    }
    ~ScopeGuard() { cleanup(); }
    ScopeGuard(ScopeGuard const&) = delete;
    auto operator=(ScopeGuard const&) -> ScopeGuard& = delete;
    ScopeGuard(ScopeGuard&&) = delete;
    auto operator=(ScopeGuard&&) -> ScopeGuard& = delete;
};

template<typename F>
ScopeGuard(F) -> ScopeGuard<F>;

// ---------------------------------------------------------------------------
// §2 — Path primitives
// ---------------------------------------------------------------------------

/// Strip trailing slashes from a path string
auto strip_trailing_slashes(std::string_view s) -> std::string_view
{
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) {
        s.remove_suffix(1);
    }
    return s;
}

/// Normalize a file path for consistent lookup
/// - Removes double slashes
/// - Resolves . and .. components using lexically_normal
/// Normalize a directory path for group key lookup.
/// - Strips trailing slashes
/// - Converts absolute paths to project-relative
/// - Resolves parent references (..) against current_dir
auto normalize_group_dir(
    std::string_view path_str,
    std::string_view current_dir,
    std::string_view source_root
) -> StringId
{
    auto cleaned = strip_trailing_slashes(path_str);
    if (cleaned.empty()) {
        return intern(".");
    }

    auto& pool = global_pool();
    auto normalized = pool.get(pup::path::normalize(cleaned));

    if (pup::path::is_absolute(normalized)) {
        normalized = pool.get(pup::path::relative(normalized, source_root));
    } else if (!current_dir.empty() && normalized != ".") {
        auto first_slash = cleaned.find('/');
        auto first_component = cleaned.substr(0, first_slash);
        if (first_component != ".") {
            normalized = pool.get(pup::path::normalize(pool.get(pup::path::join(current_dir, normalized))));
        }
    }

    return (normalized.empty() || normalized == ".") ? intern(".") : intern(normalized);
}

/// Parsed group reference from a path like "../include/<gen-headers>"
struct GroupReference {
    std::string_view group_name;
    StringId group_dir;
};

/// Check if a path is an order-only group reference (ends with <name>)
/// Examples: "<group>", "path/<group>", "../include/<gen-headers>"
auto is_order_only_group_reference(std::string_view path) -> bool
{
    auto lt_pos = path.rfind('<');
    return lt_pos != std::string_view::npos && !path.empty() && path.back() == '>';
}

/// Parse a group reference from a path expression
/// Returns nullopt if the path doesn't contain a valid <group> suffix
auto parse_group_reference(
    std::string_view path,
    std::string_view current_dir,
    std::string_view source_root
) -> std::optional<GroupReference>
{
    auto lt_pos = path.rfind('<');
    auto gt_pos = path.rfind('>');
    if (lt_pos == std::string_view::npos || gt_pos == std::string_view::npos
        || gt_pos != path.size() - 1 || gt_pos <= lt_pos) {
        return std::nullopt;
    }
    auto group_name = path.substr(lt_pos + 1, gt_pos - lt_pos - 1);
    if (group_name.empty()) {
        return std::nullopt;
    }
    auto dir_part = path.substr(0, lt_pos);
    auto group_dir = normalize_group_dir(dir_part, current_dir, source_root);
    return GroupReference { group_name, group_dir };
}

/// Normalize a path that may point to the output directory to its canonical form.
/// In cross-project builds, paths like "../../pup/build/busybox-test/include/autoconf.h"
/// may be normalized to "../pup/build/busybox-test/include/autoconf.h" when combined with
/// a subdirectory, causing strip_path_prefix to fail. This function resolves such paths
/// by checking if they point to output_root and returning the relative path within it.
auto normalize_to_output_relative(
    std::string_view path,
    std::string_view source_root,
    std::string_view output_root
) -> std::string_view
{
    if (auto resolved = pup::resolve_under_root(path, source_root, output_root)) {
        return global_pool().get(*resolved);
    }
    return path;
}

/// Trigger demand-driven parsing for a directory if it contains a Tupfile.
/// This is used when referencing cross-directory groups or generated files.
auto request_demand_driven_parse(
    parser::EvalContext const& eval,
    std::string_view dir_path
) -> void
{
    if (eval.request_directory && eval.available_tupfile_dirs) {
        auto dir_id = global_pool().find(dir_path);
        if (dir_id != StringId::Empty && std::binary_search(eval.available_tupfile_dirs->begin(), eval.available_tupfile_dirs->end(), dir_id)) {
            (void)eval.request_directory(dir_path);
        }
    }
}

// ---------------------------------------------------------------------------
// §3 — Path transform context
// ---------------------------------------------------------------------------

/// Context for transforming paths to Tupfile-relative coordinates.
///
/// Commands execute from the Tupfile's source directory, so all paths in commands
/// must be relative to that directory. With node traversal:
/// - Outputs are already variant-mapped (e.g., "build/src/foo.o")
/// - Inputs are source-root-relative (e.g., "src/lib/bar.c")
/// The transform functions below convert both to Tupfile-relative paths.
struct PathTransformContext {
    StringId source_to_root;
    StringId current_dir_id;
    StringId source_root;
    StringId config_root;
    StringId output_root;
    StringId canonical_cwd;
};

auto make_transform_context(BuilderContext const& ctx) -> PathTransformContext
{
    auto& pool = global_pool();
    auto canonical_cwd = StringId::Empty;
    if (!is_empty(ctx.options.source_root) && !is_empty(ctx.options.output_root)
        && str(ctx.options.source_root) != str(ctx.options.output_root)) {
        auto joined_sv = pool.get(pup::path::join(str(ctx.options.source_root), str(ctx.current_dir)));
        auto r = pup::platform::canonical(joined_sv);
        if (r) {
            canonical_cwd = *r;
        }
    }

    return PathTransformContext {
        .source_to_root = pup::compute_source_to_root(str(ctx.current_dir)),
        .current_dir_id = ctx.current_dir,
        .source_root = ctx.options.source_root,
        .config_root = ctx.options.config_root,
        .output_root = ctx.options.output_root,
        .canonical_cwd = canonical_cwd,
    };
}

/// Compute a canonical relative path from the source CWD to a build-tree file.
/// Resolves symlinks in the source tree so that `../` components in command paths
/// navigate correctly from the physical CWD (which the OS uses after resolving symlinks).
auto make_canonical_relative(PathTransformContext const& tc, std::string_view path) -> StringId
{
    auto& pool = global_pool();
    auto joined_sv = pool.get(pup::path::join(str(tc.source_root), path));
    auto abs = pup::platform::canonical(joined_sv);
    if (abs) {
        return pup::path::relative(pool.get(*abs), pool.get(tc.canonical_cwd));
    }
    return pup::path::relative(pool.get(pup::path::normalize(joined_sv)), pool.get(tc.canonical_cwd));
}

/// Transform an input path to Tupfile-relative for command expansion.
/// Input paths are source-relative. For Generated/Ghost files under BUILD_ROOT_ID,
/// we need to use get_full_path() to get the path including the build root prefix.
auto transform_input_path(
    BuildGraph& bs,
    PathTransformContext const& tc,
    std::string_view inp
) -> StringId
{
    auto& pool = global_pool();
    auto source_to_root = str(tc.source_to_root);
    auto current_dir_sv = str(tc.current_dir_id);

    if (auto path_id = bs.graph.paths.find_path(inp, pool, PathId::BuildRoot)) {
        if (auto const* hit = bs.graph.path_to_node.find(to_underlying(*path_id))) {
            auto full_path_sv = get_full_path(bs.graph, NodeId { *hit }, bs.path_cache);
            if (!full_path_sv.empty()) {
                if (!is_empty(tc.canonical_cwd) && full_path_sv.starts_with("..")) {
                    return make_canonical_relative(tc, full_path_sv);
                }
                return pup::make_source_relative(full_path_sv, source_to_root, current_dir_sv);
            }
        }
    }

    auto build_root_name = get_build_root_name(bs.graph);
    if (!build_root_name.empty()) {
        auto build_path = pool.get(pup::path::join(str(tc.output_root), inp));
        if (pup::platform::exists(build_path)) {
            auto full_path_id = pup::path::join(build_root_name, inp);
            auto full_path_sv = pool.get(full_path_id);
            if (!is_empty(tc.canonical_cwd) && full_path_sv.starts_with("..")) {
                return make_canonical_relative(tc, full_path_sv);
            }
            return pup::make_source_relative(full_path_sv, source_to_root, current_dir_sv);
        }
    }

    if (!is_empty(tc.config_root) && tc.config_root != tc.source_root) {
        auto config_path_sv = pool.get(pup::path::join(str(tc.config_root), inp));
        if (pup::platform::exists(config_path_sv)) {
            auto source_cwd_sv = pool.get(pup::path::join(str(tc.source_root), current_dir_sv));
            auto canonical_source = pup::platform::canonical(source_cwd_sv);
            auto canonical_config = pup::platform::canonical(config_path_sv);
            if (canonical_source && canonical_config) {
                return pup::path::relative(pool.get(*canonical_config), pool.get(*canonical_source));
            }
        }
    }

    return pup::make_source_relative(inp, source_to_root, current_dir_sv);
}

/// Transform an output path to Tupfile-relative for command expansion.
/// Outputs are already stored at variant-mapped paths (e.g., "build/src/main.o").
/// This function just converts to Tupfile-relative (e.g., "../../build/src/main.o").
auto transform_output_path(
    PathTransformContext const& tc,
    std::string_view out
) -> StringId
{
    if (!is_empty(tc.canonical_cwd) && out.starts_with("..")) {
        return make_canonical_relative(tc, out);
    }
    return pup::make_source_relative(out, str(tc.source_to_root), str(tc.current_dir_id));
}

// ---------------------------------------------------------------------------
// §4 — Node resolution and creation
// ---------------------------------------------------------------------------

// ============================================================================
// Node-Traversal Path Resolution
// ============================================================================
// Path resolution uses graph traversal instead of string manipulation:
// - ".." → walk to parent node
// - "name" → find/create child node
//
// This naturally unifies input and output path resolution because both traverse
// to the same node when the paths are equivalent (e.g., $(B)/include/header.h
// from an input and the variant-mapped output both resolve to the same node).

auto resolve_input_node(
    BuilderContext& ctx,
    std::string_view path
) -> Result<NodeId>
{
    // Input paths are already source-relative from expand_inputs() which normalizes them
    // by combining with current_dir. No further normalization needed here.

    // Track whether the path originally had the build prefix or pointed to output_root.
    // This indicates the path should reference a generated file, not a source file.
    auto had_build_prefix = false;

    // For variant builds, paths like "build/include/header.h" (from $(B)/include/header.h)
    // already have the build root prefix. Strip it to get source-relative paths.
    auto build_root_name = get_build_root_name(ctx.state->graph);
    auto normalized_path = global_pool().get(pup::strip_path_prefix(path, build_root_name));
    if (normalized_path != path) {
        had_build_prefix = true;
    }

    if (normalized_path.starts_with("..")) {
        auto before = normalized_path;
        normalized_path = normalize_to_output_relative(
            normalized_path, str(ctx.options.source_root), str(ctx.options.output_root)
        );
        if (before != normalized_path) {
            had_build_prefix = true;
        }
    }

    // With BUILD_ROOT_ID model:
    // - Source files are under SOURCE_ROOT_ID (0) at source-relative paths
    // - Generated/Ghost files are under BUILD_ROOT_ID at source-relative paths

    auto& pool = global_pool();

    // First check if node exists under BUILD_ROOT_ID (generated files)
    if (auto build_path = ctx.state->graph.paths.find_path(normalized_path, pool, PathId::BuildRoot)) {
        if (auto const* hit = ctx.state->graph.path_to_node.find(to_underlying(*build_path))) {
            return NodeId { *hit };
        }
    }

    // If path had build prefix, it's referencing a generated file. Even if a source file
    // exists at the same path, create a Ghost node under BUILD_ROOT_ID so it can be
    // upgraded to Generated when the output rule is processed.
    if (had_build_prefix) {
        auto path_id = ctx.state->graph.paths.intern_path(normalized_path, pool, PathId::BuildRoot);
        return ensure_file_node(ctx.state->graph, path_id, NodeType::Ghost);
    }

    // Check under SOURCE_ROOT_ID (source files)
    if (auto source_path = ctx.state->graph.paths.find_path(normalized_path, pool, PathId::SourceRoot)) {
        if (auto const* hit = ctx.state->graph.path_to_node.find(to_underlying(*source_path))) {
            return NodeId { *hit };
        }
    }

    // Node doesn't exist - check filesystem to determine type
    auto source_path = pool.get(pup::path::join(str(ctx.options.source_root), normalized_path));
    if (pup::platform::exists(source_path)) {
        auto path_id = ctx.state->graph.paths.intern_path(normalized_path, pool, PathId::SourceRoot);
        return ensure_file_node(ctx.state->graph, path_id, NodeType::File);
    }

    // In 3-tree builds, files may live in config_root (alongside Tupfiles) rather than
    // source_root. Check config_root as a fallback for source file resolution.
    if (!is_empty(ctx.options.config_root) && str(ctx.options.config_root) != str(ctx.options.source_root)) {
        auto config_path_sv = pool.get(pup::path::join(str(ctx.options.config_root), normalized_path));
        if (pup::platform::exists(config_path_sv)) {
            auto path_id = ctx.state->graph.paths.intern_path(normalized_path, pool, PathId::SourceRoot);
            return ensure_file_node(ctx.state->graph, path_id, NodeType::File);
        }
    }

    // Check if file exists in build directory (e.g., tup.config, or already-generated files)
    auto build_path_sv = pool.get(pup::path::join(str(ctx.options.output_root), normalized_path));
    if (pup::platform::exists(build_path_sv)) {
        auto path_id = ctx.state->graph.paths.intern_path(normalized_path, pool, PathId::BuildRoot);
        return ensure_file_node(ctx.state->graph, path_id, NodeType::Ghost);
    }

    // File doesn't exist anywhere - create Ghost node under BUILD_ROOT_ID
    auto path_id = ctx.state->graph.paths.intern_path(normalized_path, pool, PathId::BuildRoot);
    return ensure_file_node(ctx.state->graph, path_id, NodeType::Ghost);
}

/// Get all files that are members of a group (via file → group edges)
/// Returns file NodeIds by finding all input edges to the group node
auto get_group_members(Graph const& graph, NodeId group_id) -> Vec<NodeId>
{
    return get_inputs(graph, group_id);
}

auto get_or_create_group_node(
    BuilderContext& ctx,
    Builder& state,
    std::string_view directory,
    std::string_view name
) -> Result<NodeId>
{
    auto key_id = to_underlying(pup::path::join(directory, name));
    auto const* cached = state.group_nodes.find(key_id);
    if (cached) {
        return *cached;
    }

    // Get or create parent directory node
    auto dir_path_id = ctx.state->graph.paths.intern_path(directory, global_pool(), PathId::SourceRoot);
    auto parent_id_result = ensure_file_node(ctx.state->graph, dir_path_id, NodeType::Directory);
    if (!parent_id_result) {
        return parent_id_result;
    }
    auto parent_id = *parent_id_result;

    // Check if group node already exists in graph (e.g., from previous Tupfile)
    // Group nodes are stored with angle-bracket name like "<gen-headers>"
    auto gb = Buf {};
    gb += '<';
    gb += name;
    gb += '>';
    auto group_basename = gb.view();
    if (auto existing = find_by_dir_name(ctx.state->graph, parent_id, group_basename)) {
        state.group_nodes.insert(key_id, *existing);
        return *existing;
    }

    // Create new group node
    auto node = FileNode {
        .type = NodeType::Group,
        .name = intern(group_basename),
        .parent_dir = parent_id,
    };

    auto result = add_file_node(ctx.state->graph, std::move(node));
    if (result) {
        state.group_nodes.insert(key_id, *result);
    }
    return result;
}

auto create_command_node(
    BuilderContext& ctx,
    Builder& state,
    std::string_view instruction,
    std::string_view display
) -> Result<NodeId>
{
    auto exported = SortedIdVec {};
    exported.merge_from(ctx.exported_vars);

    auto node = CommandNode {
        .display = intern(display),
        .source_dir = ctx.current_dir,
        .instruction_id = intern(instruction),
        .exported_vars = std::move(exported),
        .guards = ctx.condition_stack,
    };

    auto cmd_id_result = add_command_node(ctx.state->graph, std::move(node));
    if (!cmd_id_result) {
        return cmd_id_result;
    }

    auto cmd_id = *cmd_id_result;

    // Add sticky edges from Tupfile and included files to this command
    for (auto src_id : ctx.sticky_sources) {
        (void)add_edge(ctx.state->graph, src_id, cmd_id, LinkType::Sticky);
    }

    // Add sticky edges from used config variables (fine-grained dependency tracking)
    auto const* cv = ctx.used_config_vars.data();
    for (std::size_t i = 0, n = ctx.used_config_vars.size(); i < n; ++i) {
        auto const* node_id = state.config_var_nodes.find(cv[i]);
        if (node_id) {
            (void)add_edge(ctx.state->graph, *node_id, cmd_id, LinkType::Sticky);
        }
    }

    // Add sticky edges from condition config variables (phi-node model)
    auto const* ccv = ctx.condition_config_vars.data();
    for (std::size_t i = 0, n = ctx.condition_config_vars.size(); i < n; ++i) {
        auto const* node_id = state.config_var_nodes.find(ccv[i]);
        if (node_id) {
            (void)add_edge(ctx.state->graph, *node_id, cmd_id, LinkType::Sticky);
        }
    }

    // Add sticky edges from condition env variables (symmetric to condition_config_vars
    // — so an env-driven guard flip changes every guarded command's identity).
    auto const* cev = ctx.condition_env_vars.data();
    for (std::size_t i = 0, n = ctx.condition_env_vars.size(); i < n; ++i) {
        auto const* node_id = state.imported_env_var_nodes.find(cev[i]);
        if (node_id) {
            (void)add_edge(ctx.state->graph, *node_id, cmd_id, LinkType::Sticky);
        }
    }

    // Add sticky edges from used imported env variables (fine-grained dependency tracking)
    auto const* uev = ctx.used_env_vars.data();
    for (std::size_t i = 0, n = ctx.used_env_vars.size(); i < n; ++i) {
        auto const* node_id = state.imported_env_var_nodes.find(uev[i]);
        if (node_id) {
            (void)add_edge(ctx.state->graph, *node_id, cmd_id, LinkType::Sticky);
        }
    }

    // Add sticky edges from exported env variables. An exported var reaches the
    // command's subprocess environment (read as bare $VAR) and so affects its output
    // even when it never appears in the command text. Recording it as a sticky edge
    // makes that dependency explicit and folds its value into the command identity,
    // so a change to the var triggers a rebuild. (Only imported vars have a value
    // node to depend on; export of an untracked env var remains out of the model.)
    auto const* exv = ctx.exported_vars.data();
    for (std::size_t i = 0, n = ctx.exported_vars.size(); i < n; ++i) {
        auto const* node_id = state.imported_env_var_nodes.find(exv[i]);
        if (node_id) {
            (void)add_edge(ctx.state->graph, *node_id, cmd_id, LinkType::Sticky);
        }
    }

    return cmd_id;
}

// ---------------------------------------------------------------------------
// §5 — Condition model
// ---------------------------------------------------------------------------

/// Check if all guards in the condition stack are satisfied (current context is active)
/// Returns true if no guards or all guards match their expected polarity
auto is_context_active(BuilderContext const& ctx) -> bool
{
    for (auto const& guard : ctx.condition_stack) {
        auto const* cond = get_condition_node(ctx.state->graph, guard.condition);
        if (cond && cond->current_value != guard.polarity) {
            return false;
        }
    }
    return true;
}

/// Check if two guard sets are mutually exclusive (cannot both be satisfied)
/// This allows phi-node conditionals where branches produce the same output
/// but only one executes based on config values.
///
/// Mutually exclusive if guards diverge at some position with complementary values.
/// Examples:
/// - [(C1, true)] vs [(C1, false), (C2, true)]: Position 0 differs, complementary -> exclusive
/// - [(C1, false), (C2, true)] vs [(C1, false), (C2, false)]: Position 1 differs, complementary -> exclusive
/// - [(C1, true)] vs [(C1, true), (C2, true)]: All shared guards identical -> NOT exclusive
auto are_guards_mutually_exclusive(
    Vec<Guard> const& guards_a,
    Vec<Guard> const& guards_b
) -> bool
{
    if (guards_a.empty() || guards_b.empty()) {
        return false;
    }

    auto min_len = std::min(guards_a.size(), guards_b.size());

    for (std::size_t i = 0; i < min_len; ++i) {
        if (guards_a[i] != guards_b[i]) {
            auto const& a = guards_a[i];
            auto const& b = guards_b[i];
            return a.condition == b.condition && a.polarity != b.polarity;
        }
    }

    return false;
}

/// Format a conditional expression as a string for the condition node
auto format_condition_expr(parser::EvalContext& eval, parser::Conditional const& cond) -> StringId
{
    auto buf = Buf {};
    switch (cond.kind) {
    case parser::Conditional::Kind::Ifdef:
        buf.fmt("ifdef({})", str(cond.var_name));
        break;
    case parser::Conditional::Kind::Ifndef:
        buf.fmt("ifndef({})", str(cond.var_name));
        break;
    case parser::Conditional::Kind::Ifeq: {
        auto lhs = parser::expand(eval, cond.lhs).value_or(StringId::Empty);
        auto rhs = parser::expand(eval, cond.rhs).value_or(StringId::Empty);
        buf.fmt("ifeq({},{})", str(lhs), str(rhs));
        break;
    }
    case parser::Conditional::Kind::Ifneq: {
        auto lhs = parser::expand(eval, cond.lhs).value_or(StringId::Empty);
        auto rhs = parser::expand(eval, cond.rhs).value_or(StringId::Empty);
        buf.fmt("ifneq({},{})", str(lhs), str(rhs));
        break;
    }
    }
    return buf.intern(global_pool());
}

// ---------------------------------------------------------------------------
// §6 — Glob expansion
// ---------------------------------------------------------------------------

/// Expand a glob pattern against filesystem and graph nodes.
/// Adds matched paths to result vector.
auto expand_glob_pattern(
    BuilderContext& ctx,
    std::string_view path,
    Vec<StringId>& result
) -> void
{
    auto& pool = global_pool();
    auto base_sv = is_empty(ctx.current_dir) ? str(ctx.options.source_root)
                                             : pool.get(pup::path::join(str(ctx.options.source_root), str(ctx.current_dir)));

    auto expanded = parser::glob_expand(path, base_sv);
    if (expanded && !expanded->empty()) {
        for (auto p : *expanded) {
            if (!is_empty(ctx.current_dir)) {
                result.push_back(pup::path::join(str(ctx.current_dir), str(p)));
            } else {
                result.push_back(p);
            }
        }
        return;
    }

    auto pattern_dir = pup::path::parent(path);
    auto abs_pattern_dir_sv = pool.get(pup::path::normalize(pool.get(pup::path::join(str(ctx.current_dir), pattern_dir))));
    request_demand_driven_parse(*ctx.eval, abs_pattern_dir_sv);

    auto pattern_path_sv = is_empty(ctx.current_dir) ? path : pool.get(pup::path::normalize(pool.get(pup::path::join(str(ctx.current_dir), path))));
    auto glob = parser::Glob { pattern_path_sv };
    auto build_root_name = get_build_root_name(ctx.state->graph);
    for (auto id : nodes_of_type(ctx.state->graph, NodeType::Generated)) {
        auto node_path_sv = get_full_path(ctx.state->graph, id, ctx.state->path_cache);
        if (node_path_sv.empty()) {
            continue;
        }
        auto match_path_sv = pool.get(pup::strip_path_prefix(node_path_sv, build_root_name));
        if (glob.matches(match_path_sv)) {
            result.push_back(pool.intern(node_path_sv));
        }
    }
}

/// Apply exclusion patterns to filter out paths from the result.
/// Handles both glob and non-glob exclusions.
auto apply_exclusions(
    BuilderContext& ctx,
    Vec<parser::PathPattern> const& patterns,
    Vec<StringId>& result
) -> void
{
    auto& pool = global_pool();
    for (auto const& pattern : patterns) {
        if (!pattern.is_exclusion && !pattern.is_output_exclusion) {
            continue;
        }

        auto paths = parser::expand_path(*ctx.eval, pattern);
        if (!paths) {
            continue;
        }

        for (auto excl_id : *paths) {
            auto excl = pool.get(excl_id);
            if (ctx.options.expand_globs && parser::has_glob_chars(excl)) {
                auto base_sv = is_empty(ctx.current_dir) ? str(ctx.options.source_root)
                                                         : pool.get(pup::path::join(str(ctx.options.source_root), str(ctx.current_dir)));
                auto expanded = parser::glob_expand(excl, base_sv);
                if (expanded && !expanded->empty()) {
                    for (auto p : *expanded) {
                        auto p_sv = str(p);
                        auto normalized = is_empty(ctx.current_dir) ? pup::path::normalize(p_sv) : pup::path::normalize(pool.get(pup::path::join(str(ctx.current_dir), p_sv)));
                        for (auto it = result.begin(); it != result.end();) {
                            if (pool.get(*it) == pool.get(normalized)) {
                                it = result.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    }
                }
            } else {
                auto normalized_excl = is_empty(ctx.current_dir) ? pup::path::normalize(excl) : pup::path::normalize(pool.get(pup::path::join(str(ctx.current_dir), excl)));
                auto normalized_sv = pool.get(normalized_excl);
                for (auto it = result.begin(); it != result.end();) {
                    if (pool.get(*it) == normalized_sv) {
                        it = result.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// §7 — Input/output expansion
// ---------------------------------------------------------------------------

auto expand_command(
    BuilderContext& ctx,
    parser::Expression const& cmd,
    parser::PatternFlags flags,
    Vec<PathId> const& outputs,
    StringId* out_instruction = nullptr
) -> Result<StringId>
{
    auto& pool = global_pool();
    auto literal = parser::expand(*ctx.eval, cmd);
    if (!literal) {
        return pup::unexpected<Error>(literal.error());
    }

    auto expanded = parser::expand(*ctx.eval, pool.get(*literal));
    if (!expanded) {
        return pup::unexpected<Error>(expanded.error());
    }

    if (out_instruction) {
        *out_instruction = *expanded;
    }

    auto tc = make_transform_context(ctx);
    auto cmd_outputs = Vec<StringId> {};
    cmd_outputs.reserve(outputs.size());
    for (auto out : outputs) {
        auto materialized = materialize_path(ctx.state->graph, out);
        cmd_outputs.push_back(transform_output_path(tc, str(materialized)));
    }

    auto primary_output_sv = cmd_outputs.empty() ? std::string_view {} : str(cmd_outputs[0]);
    flags.output = primary_output_sv;
    flags.output_base = parser::path_basename(primary_output_sv);
    auto outputs_sv = Vec<std::string_view> {};
    outputs_sv.reserve(cmd_outputs.size());
    for (auto id : cmd_outputs) {
        outputs_sv.push_back(str(id));
    }
    flags.all_outputs = std::move(outputs_sv);

    auto pattern_result = parser::expand_pattern(*ctx.eval, pool.get(*expanded), flags);
    if (!pattern_result) {
        return pup::unexpected<Error>(pattern_result.error());
    }
    return *pattern_result;
}

/// Lookup a bang macro from the expanded command text.
/// Returns pointer to the macro definition, or nullptr if command doesn't reference a macro.
auto lookup_bang_macro(
    BuilderContext& ctx,
    parser::Expression const& command,
    parser::PatternFlags const& flags
) -> Result<BangMacroDef const*>
{
    auto expanded_cmd = expand_command(ctx, command, flags, {});
    if (!expanded_cmd) {
        return pup::unexpected<Error>(expanded_cmd.error());
    }

    auto cmd_sv = str(*expanded_cmd);
    while (!cmd_sv.empty() && (cmd_sv.front() == ' ' || cmd_sv.front() == '\t')) {
        cmd_sv.remove_prefix(1);
    }

    if (cmd_sv.empty() || cmd_sv[0] != '!') {
        return nullptr;
    }

    auto name_end = cmd_sv.find_first_of(" \t", 1);
    auto macro_name = (name_end == std::string_view::npos)
        ? cmd_sv.substr(1)
        : cmd_sv.substr(1, name_end - 1);

    auto key = to_underlying(intern(macro_name));
    auto it = std::lower_bound(ctx.macros.begin(), ctx.macros.end(), key, [](auto const& p, auto k) { return p.first < k; });
    if (it == ctx.macros.end() || it->first != key) {
        auto err = Buf {};
        err.fmt("Unknown bang macro: !{}", macro_name);
        return make_error<BangMacroDef const*>(ErrorCode::UnknownMacro, err.view());
    }

    return &it->second;
}

auto expand_outputs(
    BuilderContext& ctx,
    Vec<parser::PathPattern> const& patterns,
    parser::PatternFlags const& flags
) -> Result<Vec<PathId>>
{
    auto& pool = global_pool();
    auto result = Vec<PathId> {};

    for (auto const& pattern : patterns) {
        if (pattern.is_group) {
            continue;
        }
        if (pattern.is_output_exclusion) {
            continue;
        }

        auto paths = parser::expand_path(*ctx.eval, pattern);
        if (!paths) {
            return pup::unexpected<Error>(paths.error());
        }

        for (auto path_id : *paths) {
            auto expanded = parser::expand_pattern(*ctx.eval, pool.get(path_id), flags);
            auto output_path_sv = expanded ? pool.get(*expanded) : pool.get(path_id);

            auto full_output_path_sv = pool.get(pup::path::normalize(pool.get(pup::path::join(str(ctx.current_dir), output_path_sv))));

            result.push_back(ctx.state->graph.paths.intern_path(full_output_path_sv, pool, PathId::BuildRoot));
        }
    }

    return result;
}

auto expand_inputs(
    BuilderContext& ctx,
    Vec<parser::PathPattern> const& patterns
) -> Result<Vec<StringId>>
{
    auto& pool = global_pool();
    auto result = Vec<StringId> {};

    for (auto const& pattern : patterns) {
        if (pattern.is_exclusion || pattern.is_output_exclusion) {
            continue;
        }

        if (pattern.is_group) {
            auto gkey = to_underlying(intern(str(pattern.group_name)));
            if (auto const* members = ctx.groups.find(gkey)) {
                for (auto id : *members) {
                    auto path_sv = get_full_path(ctx.state->graph, id, ctx.state->path_cache);
                    if (!path_sv.empty()) {
                        result.push_back(intern(path_sv));
                    }
                }
            }
            continue;
        }

        if (pattern.is_order_only_group) {
            auto group_dir = StringId::Empty;

            if (!pattern.path.empty()) {
                auto expanded = parser::expand(*ctx.eval, pattern.path);
                if (expanded) {
                    group_dir = normalize_group_dir(str(*expanded), str(ctx.current_dir), str(ctx.options.source_root));
                    request_demand_driven_parse(*ctx.eval, str(group_dir));
                }
            } else {
                group_dir = is_empty(ctx.current_dir) ? intern(".") : ctx.current_dir;
            }

            auto group_name_sv = str(pattern.group_name);
            auto ref_buf = Buf {};
            auto group_dir_sv = str(group_dir);
            if (group_dir_sv.empty()) {
                ref_buf += '<';
                ref_buf += group_name_sv;
                ref_buf += '>';
            } else {
                ref_buf += group_dir_sv;
                ref_buf += "/<";
                ref_buf += group_name_sv;
                ref_buf += '>';
            }
            result.push_back(ref_buf.intern(pool));
            continue;
        }

        auto paths = parser::expand_path(*ctx.eval, pattern);
        if (!paths) {
            return pup::unexpected<Error>(paths.error());
        }

        for (auto path_id : *paths) {
            auto path_sv = pool.get(path_id);
            auto group_ref = parse_group_reference(path_sv, str(ctx.current_dir), str(ctx.options.source_root));
            if (group_ref) {
                request_demand_driven_parse(*ctx.eval, str(group_ref->group_dir));
                result.push_back(path_id);
                continue;
            }
            if (!is_empty(ctx.current_dir)) {
                result.push_back(pup::path::normalize(pool.get(pup::path::join(str(ctx.current_dir), path_sv))));
            } else {
                result.push_back(path_id);
            }

            if (ctx.options.expand_globs && parser::has_glob_chars(path_sv)) {
                expand_glob_pattern(ctx, path_sv, result);
            } else if (!parser::has_glob_chars(path_sv)) {
                auto full_path_sv = pool.get(pup::path::join(pool.get(pup::path::join(str(ctx.options.source_root), str(ctx.current_dir))), path_sv));
                if (!pup::platform::exists(full_path_sv)) {
                    auto file_dir = pup::path::parent(path_sv);
                    auto abs_file_dir_sv = pool.get(pup::path::normalize(pool.get(pup::path::join(str(ctx.current_dir), file_dir))));
                    request_demand_driven_parse(*ctx.eval, abs_file_dir_sv);
                }
            }
        }
    }

    apply_exclusions(ctx, patterns, result);

    return result;
}

// ---------------------------------------------------------------------------
// §8 — Include/import/export
// ---------------------------------------------------------------------------

// Forward declaration (mutual recursion: include_single_file → process_statement → process_conditional → process_statement)
auto process_statement(
    BuilderContext& ctx,
    Builder& state,
    parser::Statement const& stmt
) -> Result<void>;

/// Collect all Tuprules.tup files from root to start_dir (root-first order).
/// Per tup semantics, include_rules includes every Tuprules.tup from the
/// project root down to the current directory. Gaps are allowed.
auto find_tuprules_files(
    std::string_view start_dir,
    std::string_view root
) -> Vec<StringId>
{
    auto& pool = global_pool();
    auto dirs = Vec<StringId> {};
    auto search_sv = start_dir;

    while (search_sv.size() >= root.size()) {
        dirs.push_back(pool.intern(search_sv));
        if (search_sv == root) {
            break;
        }
        auto par = pup::path::parent(search_sv);
        if (par == search_sv || par.empty()) {
            break;
        }
        search_sv = par;
    }

    std::reverse(dirs.begin(), dirs.end());

    auto results = Vec<StringId> {};
    for (auto dir_id : dirs) {
        auto tuprules_id = pup::path::join(pool.get(dir_id), "Tuprules.tup");
        if (pup::platform::exists(pool.get(tuprules_id))) {
            results.push_back(tuprules_id);
        }
    }

    return results;
}

/// Resolve an explicit include path (not include_rules)
/// Returns the resolved path or an error
auto resolve_include_path(
    BuilderContext& ctx,
    std::string_view include_root,
    parser::Expression const& path_expr
) -> Result<StringId>
{
    auto& pool = global_pool();
    auto path_result = parser::expand(*ctx.eval, path_expr);
    if (!path_result) {
        return pup::unexpected<Error>(path_result.error());
    }

    auto path_sv = pool.get(*path_result);
    auto base_sv = pool.get(pup::path::join(include_root, str(ctx.current_dir)));
    auto resolved_id = pup::path::join(base_sv, path_sv);
    if (!pup::platform::exists(pool.get(resolved_id))) {
        auto err = Buf {};
        err.fmt("Include file not found: {}", path_sv);
        return make_error<StringId>(ErrorCode::IncludeNotFound, err.view());
    }
    return resolved_id;
}

auto include_single_file(
    BuilderContext& ctx,
    Builder& state,
    std::string_view include_root,
    std::string_view include_path,
    bool is_rules
) -> Result<void>
{
    auto include_path_id = to_underlying(intern(include_path));
    if (ctx.included_files.contains(include_path_id)) {
        return {};
    }
    ctx.included_files.insert(include_path_id);

    auto inc_rel = global_pool().get(pup::path::relative(include_path, include_root));
    auto inc_path_id = ctx.state->graph.paths.intern_path(inc_rel, global_pool(), PathId::SourceRoot);
    auto inc_node_result = ensure_file_node(ctx.state->graph, inc_path_id, NodeType::File);
    if (inc_node_result) {
        ctx.sticky_sources.push_back(*inc_node_result);
    }

    auto source_result = pup::platform::read_file(include_path);
    if (!source_result) {
        auto err = Buf {};
        err.fmt("Cannot open include file: {}", include_path);
        return make_error<void>(ErrorCode::IoError, err.view());
    }
    auto source = std::move(*source_result);

    auto parse_result = parser::parse_tupfile(source.view(), include_path);
    if (!parse_result.success()) {
        for (auto const& err : parse_result.errors) {
            auto err_msg = str(err.message);
            fprintf(stderr, "%.*s:%d:%d: error: %.*s\n", static_cast<int>(include_path.size()), include_path.data(), err.location.line, err.location.column, static_cast<int>(err_msg.size()), err_msg.data());
        }
        auto err = Buf {};
        err.fmt("Parse error in include file: {}", include_path);
        return make_error<void>(ErrorCode::ParseError, err.view());
    }

    auto old_tup_cwd = StringId::Empty;
    if (is_rules && ctx.eval) {
        old_tup_cwd = ctx.eval->tup_cwd;
        auto include_dir = pup::path::parent(include_path);
        auto rel_path = global_pool().get(pup::path::relative(include_dir, global_pool().get(pup::path::join(include_root, str(ctx.current_dir)))));
        ctx.eval->tup_cwd = rel_path.empty() ? intern(".") : intern(rel_path);
    }

    auto old_current_file = ctx.current_file;
    ctx.current_file = intern(include_path);

    for (auto const& stmt : parse_result.tupfile.statements) {
        auto result = process_statement(ctx, state, *stmt);
        if (!result) {
            ctx.current_file = old_current_file;
            if (is_rules && ctx.eval) {
                ctx.eval->tup_cwd = old_tup_cwd;
            }
            return pup::unexpected<Error>(result.error());
        }
    }

    ctx.current_file = old_current_file;
    if (is_rules && ctx.eval) {
        ctx.eval->tup_cwd = old_tup_cwd;
    }

    return {};
}

/// Apply pending weak assignments (??=) - last wins for each variable name
/// Iterates in reverse order so later assignments take precedence
auto apply_pending_weak_assignments(BuilderContext& ctx, Builder& state) -> void
{
    if (!ctx.vars || ctx.pending_weak_assignments.empty()) {
        return;
    }
    for (auto it = ctx.pending_weak_assignments.rbegin();
         it != ctx.pending_weak_assignments.rend();
         ++it) {
        if (!ctx.vars->contains(str(it->name))) {
            ctx.vars->set(str(it->name), str(it->value));
            auto name_id = it->name;
            if (!it->config_deps.empty()) {
                state.var_config_deps.get_or_create(name_id) = std::move(it->config_deps);
            }
            if (!it->env_deps.empty()) {
                state.var_env_deps.get_or_create(name_id) = std::move(it->env_deps);
            }
        }
    }
    ctx.pending_weak_assignments.clear();
}

// ---------------------------------------------------------------------------
// §9 — Directive processors
// ---------------------------------------------------------------------------

auto process_export(
    BuilderContext& ctx,
    parser::Export const& exp
) -> Result<void>
{
    // Per tup manual: "adds the environment variable VARIABLE to the export
    // list for future :-rules"
    ctx.exported_vars.insert(to_underlying(intern(str(exp.var_name))));
    return {};
}

/// Find or create a `NodeType::Variable` node under the env-var directory for the
/// given env var name and value. Updates the existing node's name and content_hash
/// if the value changed. Returns the node id, or nullopt if the env-var directory
/// has not been initialized yet. Used by `process_import` and the `on_env_var_used`
/// callback so that any env var the build depends on — whether explicitly imported
/// or auto-resolved like TUP_PLATFORM/TUP_ARCH — gets the same tracking node, and
/// the existing sticky-edge machinery in create_command_node folds it into command
/// identity.
auto ensure_env_var_node(
    BuilderContext& ctx,
    Builder& state,
    std::string_view var_name,
    std::string_view value
) -> std::optional<NodeId>
{
    if (state.env_var_dir_id == INVALID_NODE_ID) {
        return std::nullopt;
    }
    auto var_name_id = to_underlying(intern(var_name));
    auto node_name_buf = Buf {};
    node_name_buf += var_name;
    node_name_buf += '=';
    node_name_buf += value;
    auto const name_id = intern(node_name_buf.view());
    auto content_hash = sha256(value);

    if (auto const* existing_node_id = state.imported_env_var_nodes.find(var_name_id)) {
        if (auto* existing = get_file_node(ctx.state->graph, *existing_node_id)) {
            if (existing->name != name_id) {
                existing->name = name_id;
                existing->content_hash = content_hash;
            }
        }
        return *existing_node_id;
    }
    auto node = FileNode {
        .type = NodeType::Variable,
        .name = name_id,
        .parent_dir = state.env_var_dir_id,
        .content_hash = content_hash,
    };
    auto result = add_file_node(ctx.state->graph, std::move(node));
    if (!result) {
        return std::nullopt;
    }
    state.imported_env_var_nodes.insert(var_name_id, *result);
    return *result;
}

auto process_import(
    BuilderContext& ctx,
    Builder& state,
    parser::Import const& imp
) -> Result<void>
{
    // Per tup manual: "sets a variable inside the Tupfile that has the value
    // of the environment variable"
    auto var_name_sv = str(imp.var_name);
    auto value_id = StringId::Empty;

    // Need null-terminated string for getenv
    auto name_buf = Buf {};
    name_buf += var_name_sv;

    // 1. Try environment first
    if (auto const* env_val = std::getenv(name_buf.c_str())) {
        value_id = intern(env_val);
    }
    // 2. Fall back to cached value from previous build (passed via options)
    else if (auto it = std::lower_bound(
                 state.options.cached_env_vars.begin(),
                 state.options.cached_env_vars.end(),
                 var_name_sv,
                 [](auto const& p, std::string_view k) { return str(p.first) < k; }
             );
             it != state.options.cached_env_vars.end() && str(it->first) == var_name_sv) {
        value_id = it->second;
    }
    // 3. Fall back to default value
    else if (imp.default_value) {
        auto expanded = parser::expand(*ctx.eval, *imp.default_value);
        if (!expanded) {
            return pup::unexpected<Error>(expanded.error());
        }
        value_id = *expanded;
    }

    auto value_sv = str(value_id);

    (void)ensure_env_var_node(ctx, state, var_name_sv, value_sv);

    if (ctx.vars) {
        ctx.vars->set(var_name_sv, value_sv);
    }

    // Track this as an imported variable for fine-grained dependency tracking
    auto var_name_id = to_underlying(intern(var_name_sv));
    state.imported_var_names.insert(var_name_id);

    return {};
}

auto process_assignment(
    BuilderContext& ctx,
    Builder& state,
    parser::Assignment const& assign
) -> Result<void>
{
    // Evaluate the variable name (may contain variable refs like foo-$(BAR))
    auto name = parser::expand(*ctx.eval, assign.name);
    if (!name) {
        return pup::unexpected<Error>(name.error());
    }

    // Save current tracking state and clear for value expansion
    // This lets us capture which config/env vars are used in the RHS
    // SortedIdVec moved-from state is empty, so no explicit clear() needed
    auto saved_config_vars = std::move(ctx.used_config_vars);
    auto saved_env_vars = std::move(ctx.used_env_vars);

    // Evaluate the value - callbacks will populate used_*_vars
    auto value = parser::expand(*ctx.eval, assign.value);
    if (!value) {
        ctx.used_config_vars = std::move(saved_config_vars);
        ctx.used_env_vars = std::move(saved_env_vars);
        return pup::unexpected<Error>(value.error());
    }

    // Capture the dependencies from RHS expansion
    auto captured_config_deps = std::move(ctx.used_config_vars);
    auto captured_env_deps = std::move(ctx.used_env_vars);

    // Restore tracking state
    ctx.used_config_vars = std::move(saved_config_vars);
    ctx.used_env_vars = std::move(saved_env_vars);

    // Config variables are read-only (loaded from tup.config), so only Regular and Node are writable
    auto* db = ctx.eval->vars;
    if (assign.var_kind == parser::VarRef::Kind::Node) {
        db = ctx.eval->node_vars;
    }

    if (!db) {
        return {};
    }

    auto name_sv = str(*name);
    auto value_sv = str(*value);
    auto value_before = intern(db->get(name_sv));
    auto is_effective = true;

    auto name_id = intern(name_sv);
    auto record_deps = [&]() {
        if (!captured_config_deps.empty()) {
            auto& deps = state.var_config_deps.get_or_create(name_id);
            if (assign.op == parser::Assignment::Op::Set
                || assign.op == parser::Assignment::Op::Define
                || assign.op == parser::Assignment::Op::SoftSet) {
                deps = std::move(captured_config_deps);
            } else if (assign.op == parser::Assignment::Op::Append) {
                deps.merge_from(captured_config_deps);
            }
        }
        if (!captured_env_deps.empty()) {
            auto& deps = state.var_env_deps.get_or_create(name_id);
            if (assign.op == parser::Assignment::Op::Set
                || assign.op == parser::Assignment::Op::Define
                || assign.op == parser::Assignment::Op::SoftSet) {
                deps = std::move(captured_env_deps);
            } else if (assign.op == parser::Assignment::Op::Append) {
                deps.merge_from(captured_env_deps);
            }
        }
    };

    switch (assign.op) {
    case parser::Assignment::Op::Set:
        db->set(name_sv, value_sv);
        record_deps();
        break;
    case parser::Assignment::Op::Append:
        db->append(name_sv, value_sv);
        record_deps();
        break;
    case parser::Assignment::Op::Define:
        db->set(name_sv, value_sv);
        record_deps();
        break;
    case parser::Assignment::Op::SoftSet:
        if (!db->contains(name_sv)) {
            db->set(name_sv, value_sv);
            record_deps();
        } else {
            is_effective = false;
        }
        break;
    case parser::Assignment::Op::WeakSet:
        ctx.pending_weak_assignments.push_back(PendingWeakAssignment {
            .name = *name,
            .value = *value,
            .config_deps = std::move(captured_config_deps),
            .env_deps = std::move(captured_env_deps),
        });
        break;
    }

    if (ctx.eval->on_var_assigned) {
        auto value_after_sv = db->get(name_sv);
        ctx.eval->on_var_assigned(
            name_sv,
            assign.op,
            str(value_before),
            value_after_sv,
            str(ctx.current_file),
            assign.location.line,
            assign.location.column,
            is_effective
        );
    }

    return {};
}

auto process_bang_macro(
    BuilderContext& ctx,
    parser::BangMacro const& macro
) -> Result<void>
{
    auto def = BangMacroDef {};
    def.name = macro.name;
    def.foreach_ = macro.foreach_;
    def.order_only_inputs = macro.order_only_inputs;
    def.command = macro.command;
    def.display = macro.display;
    def.outputs = macro.outputs;
    def.extra_outputs = macro.extra_outputs;
    def.output_group = macro.output_group;
    def.output_order_only_group = macro.output_order_only_group;
    def.output_order_only_group_dir = macro.output_order_only_group_dir;
    auto key = to_underlying(intern(str(macro.name)));
    auto it = std::lower_bound(ctx.macros.begin(), ctx.macros.end(), key, [](auto const& p, auto k) { return p.first < k; });
    if (it != ctx.macros.end() && it->first == key) {
        it->second = std::move(def);
    } else {
        ctx.macros.insert(it, { key, std::move(def) });
    }

    return {};
}

auto process_conditional(
    BuilderContext& ctx,
    Builder& state,
    parser::Conditional const& cond
) -> Result<void>
{
    // Save and clear used_*_vars to capture which vars the condition uses.
    // Env vars are tracked symmetrically with config vars so that an env-driven
    // guard flip (e.g. `ifeq ($(TUP_PLATFORM),win32)`) folds into the identity of
    // every command in the conditional block.
    auto saved_config_vars = std::move(ctx.used_config_vars);
    auto saved_used_env_vars = std::move(ctx.used_env_vars);

    // Evaluate condition value - this may use config vars like @(MODE) or env vars
    auto condition_true = parser::evaluate_condition(*ctx.eval, cond);

    // Capture vars used in the condition expression
    auto condition_vars = std::move(ctx.used_config_vars);
    auto condition_env_vars_used = std::move(ctx.used_env_vars);
    ctx.used_config_vars = std::move(saved_config_vars);
    ctx.used_env_vars = std::move(saved_used_env_vars);

    // Save condition_*_vars, then merge in condition-specific vars
    auto saved_condition_vars = std::move(ctx.condition_config_vars);
    auto saved_condition_env_vars = std::move(ctx.condition_env_vars);
    // Rebuild: copy saved entries + merge condition_vars
    auto const* d = saved_condition_vars.data();
    for (std::size_t i = 0, n = saved_condition_vars.size(); i < n; ++i) {
        ctx.condition_config_vars.insert(d[i]);
    }
    ctx.condition_config_vars.merge_from(condition_vars);
    auto const* de = saved_condition_env_vars.data();
    for (std::size_t i = 0, n = saved_condition_env_vars.size(); i < n; ++i) {
        ctx.condition_env_vars.insert(de[i]);
    }
    ctx.condition_env_vars.merge_from(condition_env_vars_used);
    auto restore_condition_vars = ScopeGuard([&] {
        ctx.condition_config_vars = std::move(saved_condition_vars);
        ctx.condition_env_vars = std::move(saved_condition_env_vars);
    });

    // Create condition node for phi-node model
    auto cond_expr = format_condition_expr(*ctx.eval, cond);
    auto cond_node = ConditionNode {
        .expression = cond_expr,
        .current_value = condition_true,
    };

    auto cond_id_result = add_condition_node(ctx.state->graph, std::move(cond_node));
    if (!cond_id_result) {
        return pup::unexpected<Error>(cond_id_result.error());
    }
    auto cond_id = *cond_id_result;

    // Helper to check if a statement should be processed in an inactive branch
    // These statements are needed even when branch is inactive:
    // - Rules: get guards attached, kept in graph for phi-node model
    // - Conditionals: may contain rules that need processing
    // - Includes: define macros needed by rules
    // - BangMacros: define macros needed by rules
    auto should_process_in_inactive_branch = [](parser::Statement const& stmt) {
        return stmt.is<parser::Rule>()
            || stmt.is<parser::Conditional>()
            || stmt.is<parser::Include>()
            || stmt.is<parser::BangMacro>();
    };

    // Helper to process a branch with given polarity
    auto process_branch = [&](
                              Vec<std::unique_ptr<parser::Statement>> const& body,
                              bool polarity,
                              bool is_active
                          ) -> Result<void> {
        ctx.condition_stack.push_back(Guard { .condition = cond_id, .polarity = polarity });
        auto pop_guard = ScopeGuard([&] { ctx.condition_stack.pop_back(); });

        for (auto const& stmt : body) {
            if (!is_active && !should_process_in_inactive_branch(*stmt)) {
                continue;
            }
            auto result = process_statement(ctx, state, *stmt);
            if (!result) {
                return pup::unexpected<Error>(result.error());
            }
        }
        return {};
    };

    // Process THEN branch (polarity=true means "condition must be true")
    auto then_result = process_branch(cond.then_body, true, condition_true);
    if (!then_result) {
        return pup::unexpected<Error>(then_result.error());
    }

    // Process ELSE branch (polarity=false means "condition must be false")
    auto else_result = process_branch(cond.else_body, false, !condition_true);
    if (!else_result) {
        return pup::unexpected<Error>(else_result.error());
    }

    return {};
}

auto process_include(
    BuilderContext& ctx,
    Builder& state,
    parser::Include const& inc
) -> Result<void>
{
    auto const& include_root = is_empty(ctx.options.config_root) ? str(ctx.options.source_root) : str(ctx.options.config_root);

    if (inc.is_rules) {
        auto tuprules_files = find_tuprules_files(global_pool().get(pup::path::join(include_root, str(ctx.current_dir))), include_root);
        for (auto tuprules_id : tuprules_files) {
            auto result = include_single_file(ctx, state, include_root, str(tuprules_id), true);
            if (!result) {
                return pup::unexpected<Error>(result.error());
            }
        }
        return {};
    }

    auto resolved = resolve_include_path(ctx, include_root, inc.path);
    if (!resolved) {
        return pup::unexpected<Error>(resolved.error());
    }
    return include_single_file(ctx, state, include_root, str(*resolved), false);
}

// ---------------------------------------------------------------------------
// §10 — Rule expansion
// ---------------------------------------------------------------------------

/// Store deferred order-only edges from groups to a command.
/// Groups can't be resolved to edges immediately because group membership
/// may grow as more Tupfiles are parsed. Edges are materialized later by
/// finalize_graph().
auto add_deferred_group_edges(
    Builder& state,
    Vec<NodeId> const& group_ids,
    NodeId cmd_id
) -> void
{
    for (auto group_id : group_ids) {
        auto edge = DeferredOrderOnlyEdge { group_id, cmd_id };
        auto pos = std::lower_bound(state.deferred_edges.begin(), state.deferred_edges.end(), edge);
        if (pos == state.deferred_edges.end() || !(*pos == edge)) {
            state.deferred_edges.insert(pos, edge);
        }
    }
}

/// Process generated rules (e.g., DEP commands for dependency scanning).
/// Creates command nodes and edges for each generated rule.
/// Group order-only dependencies are passed as resolved NodeIds from the
/// parent rule's pre-resolution, avoiding string→NodeId re-parsing.
auto process_generated_rules(
    BuilderContext& ctx,
    Builder& state,
    Vec<GeneratedRule> const& generated_rules,
    NodeId parent_cmd_id,
    Vec<NodeId> const& deferred_groups
) -> void
{
    auto& pool = global_pool();
    for (auto const& gen_rule : generated_rules) {
        auto gen_cmd_id = create_command_node(ctx, state, pool.get(gen_rule.command), pool.get(gen_rule.display));
        if (!gen_cmd_id) {
            continue;
        }

        // Create edges from inputs to generated command and collect operands
        auto gen_input_ids = Vec<NodeId> {};
        for (auto input_id_val : gen_rule.inputs) {
            auto input_id = resolve_input_node(ctx, pool.get(input_id_val));
            if (input_id) {
                (void)add_edge(ctx.state->graph, *input_id, *gen_cmd_id);
                gen_input_ids.push_back(*input_id);
            }
        }

        // Create order-only file edges (group refs filtered out upstream)
        for (auto oi_id : gen_rule.order_only_inputs) {
            auto oi = pool.get(oi_id);
            if (is_order_only_group_reference(oi) || parser::has_glob_chars(oi)) {
                continue;
            }
            auto oi_node = resolve_input_node(ctx, oi);
            if (oi_node) {
                (void)add_edge(ctx.state->graph, *oi_node, *gen_cmd_id, LinkType::OrderOnly);
            }
        }

        // Propagate parent's group dependencies to this generated command
        add_deferred_group_edges(state, deferred_groups, *gen_cmd_id);

        // Add edge from generated command to parent command (dep-scan runs before compile)
        (void)add_edge(ctx.state->graph, *gen_cmd_id, parent_cmd_id);

        // Store generated rule info and operands on the node.
        // outputs intentionally left empty: generated rules are dep-scan commands
        // whose output is captured via generated_output, not %o expansion.
        if (auto* node = get_command_node(ctx.state->graph, *gen_cmd_id)) {
            node->generated_output = gen_rule.outputs.empty() ? GeneratedOutput {} : gen_rule.outputs[0];
            node->output_action = gen_rule.action;
            node->parent_command = gen_rule.parent_command;
            node->inputs = std::move(gen_input_ids);
        }
    }
}

auto expand_rule(
    BuilderContext& ctx,
    Builder& state,
    parser::Rule const& rule,
    Vec<StringId> const& inputs
) -> Result<void>
{
    ctx.used_config_vars.clear();
    ctx.used_env_vars.clear();

    auto glob_pattern = StringId::Empty;
    auto file_inputs = Vec<StringId> {};
    for (auto inp : inputs) {
        if (parser::has_glob_chars(str(inp))) {
            glob_pattern = inp;
        } else {
            file_inputs.push_back(inp);
        }
    }

    auto tc = make_transform_context(ctx);
    auto cmd_inputs = Vec<StringId> {};
    cmd_inputs.reserve(file_inputs.size());
    for (auto inp : file_inputs) {
        cmd_inputs.push_back(transform_input_path(*ctx.state, tc, str(inp)));
    }

    auto primary_input_sv = cmd_inputs.empty() ? std::string_view {} : str(cmd_inputs[0]);
    auto current_dir_name = is_empty(ctx.current_dir)
        ? std::string_view { "." }
        : pup::path::filename(str(ctx.current_dir));
    auto glob_match_id = is_empty(glob_pattern) ? StringId::Empty
                                                : parser::glob_match_extract(str(glob_pattern), primary_input_sv);

    auto all_inputs_sv = Vec<std::string_view> {};
    all_inputs_sv.reserve(cmd_inputs.size());
    for (auto id : cmd_inputs) {
        all_inputs_sv.push_back(str(id));
    }

    auto flags = parser::PatternFlags {
        .input = primary_input_sv,
        .input_base = parser::path_basename(primary_input_sv),
        .input_noext = parser::path_stem(primary_input_sv),
        .input_ext = parser::path_extension(primary_input_sv),
        .input_dir = current_dir_name,
        .glob_match = str(glob_match_id),
        .all_inputs = std::move(all_inputs_sv),
    };

    // Early macro lookup - needed to process macro's order_only_inputs for demand-driven parsing
    auto macro_result = lookup_bang_macro(ctx, rule.command, flags);
    if (!macro_result) {
        return pup::unexpected<Error>(macro_result.error());
    }
    auto macro_ptr = *macro_result;

    // Resolve effective fields: merge rule + macro once.
    // - command/display: macro wins (it IS the command template)
    // - outputs: macro fills in only if rule has none
    // - groups: rule wins, macro is fallback
    // - order-only inputs: concatenated (handled below via all_order_only)
    auto const& eff_command = macro_ptr ? macro_ptr->command : rule.command;
    auto const* eff_display = macro_ptr && macro_ptr->display ? &*macro_ptr->display
        : rule.display                                        ? &*rule.display
                                                              : nullptr;
    auto const& eff_outputs = macro_ptr && rule.outputs.empty() ? macro_ptr->outputs
                                                                : rule.outputs;
    auto eff_output_group = std::optional<StringId> {};
    if (rule.output_group) {
        eff_output_group = rule.output_group;
    } else if (macro_ptr && macro_ptr->output_group) {
        eff_output_group = macro_ptr->output_group;
    }
    auto eff_output_oo_group = std::optional<StringId> {};
    if (rule.output_order_only_group) {
        eff_output_oo_group = rule.output_order_only_group;
    } else if (macro_ptr && macro_ptr->output_order_only_group) {
        eff_output_oo_group = macro_ptr->output_order_only_group;
    }
    auto const* eff_oo_group_dir = rule.output_order_only_group_dir ? &*rule.output_order_only_group_dir
        : macro_ptr && macro_ptr->output_order_only_group_dir       ? &*macro_ptr->output_order_only_group_dir
                                                                    : nullptr;

    // Pre-resolve order-only group references so %<group> can expand them in commands
    // This handles cross-directory groups like: | ../include/<gen-headers> |> cat %<gen-headers>
    // Stores known group names (sorted); the resolver constructs %<name> on the fly.
    auto rule_order_only_group_names = SortedIdVec {};

    // Track group NodeIds for deferred edge creation
    // Groups are first-class nodes; edges created after all Tupfiles are parsed
    auto deferred_group_ids = NodeIdMap32 {};
    auto deferred_group_vec = Vec<NodeId> {};

    // Merge rule + macro order-only inputs once (reused for group pre-resolution and expansion)
    auto all_order_only = rule.order_only_inputs;
    if (macro_ptr && !macro_ptr->order_only_inputs.empty()) {
        all_order_only.insert(all_order_only.end(), macro_ptr->order_only_inputs.begin(), macro_ptr->order_only_inputs.end());
    }

    // Check all inputs (regular + order-only) for group references.
    // In tup, <group> references are always order-only even when in the inputs section.
    auto all_inputs = Vec<parser::PathPattern> {};
    all_inputs.insert(all_inputs.end(), rule.inputs.begin(), rule.inputs.end());
    all_inputs.insert(all_inputs.end(), all_order_only.begin(), all_order_only.end());

    for (auto const& pattern : all_inputs) {
        if (pattern.is_order_only_group) {
            auto group_dir = StringId::Empty;
            if (!pattern.path.empty()) {
                auto expanded = parser::expand(*ctx.eval, pattern.path);
                if (expanded) {
                    group_dir = normalize_group_dir(str(*expanded), str(ctx.current_dir), str(ctx.options.source_root));
                }
            } else {
                group_dir = is_empty(ctx.current_dir) ? intern(".") : ctx.current_dir;
            }

            request_demand_driven_parse(*ctx.eval, str(group_dir));

            auto group_name_sv = str(pattern.group_name);
            auto group_id_result = get_or_create_group_node(ctx, state, str(group_dir), group_name_sv);
            if (!group_id_result) {
                continue;
            }
            auto group_id = *group_id_result;

            rule_order_only_group_names.insert(to_underlying(intern(group_name_sv)));
            if (!deferred_group_ids.contains(group_id)) {
                deferred_group_ids.set(group_id, 1);
                deferred_group_vec.push_back(group_id);
            }
        } else if (!pattern.path.empty()) {
            auto expanded = parser::expand(*ctx.eval, pattern.path);
            if (!expanded) {
                continue;
            }
            auto group_ref = parse_group_reference(str(*expanded), str(ctx.current_dir), str(ctx.options.source_root));
            if (group_ref) {
                request_demand_driven_parse(*ctx.eval, str(group_ref->group_dir));

                auto group_id_result = get_or_create_group_node(ctx, state, str(group_ref->group_dir), group_ref->group_name);
                if (!group_id_result) {
                    continue;
                }
                auto group_id = *group_id_result;

                rule_order_only_group_names.insert(to_underlying(intern(group_ref->group_name)));
                if (!deferred_group_ids.contains(group_id)) {
                    deferred_group_ids.set(group_id, 1);
                    deferred_group_vec.push_back(group_id);
                }
            }
        }
    }

    // Override resolve_order_only_group for this rule's command expansion.
    // All %<group> patterns are preserved literally for deferred resolution.
    // ScopeGuard ensures restoration even on early returns.
    auto original_resolver = std::move(ctx.eval->resolve_order_only_group);
    auto resolver_guard = ScopeGuard([&] { ctx.eval->resolve_order_only_group = std::move(original_resolver); });
    ctx.eval->resolve_order_only_group = [&rule_order_only_group_names, &deferred_group_ids, &deferred_group_vec, &ctx, &state](std::string_view name
                                         ) -> Vec<StringId> {
        auto name_key = to_underlying(intern(name));
        if (rule_order_only_group_names.contains(name_key)) {
            auto marker = Buf {};
            marker += "%<";
            marker += name;
            marker += '>';
            return { marker.intern(global_pool()) };
        }
        auto dir = is_empty(ctx.current_dir) ? std::string_view { "." } : str(ctx.current_dir);
        auto key_buf = Buf {};
        key_buf += dir;
        key_buf += '/';
        key_buf += name;
        auto key_id = to_underlying(intern(key_buf.view()));
        auto const* node_id = state.group_nodes.find(key_id);
        if (node_id) {
            if (!deferred_group_ids.contains(*node_id)) {
                deferred_group_ids.set(*node_id, 1);
                deferred_group_vec.push_back(*node_id);
            }
            rule_order_only_group_names.insert(name_key);
            auto marker = Buf {};
            marker += "%<";
            marker += name;
            marker += '>';
            return { marker.intern(global_pool()) };
        }
        return {};
    };

    auto cmd_text = StringId::Empty;
    auto display = StringId::Empty;
    auto instruction_pattern = StringId::Empty;

    auto outputs = expand_outputs(ctx, eff_outputs, flags);
    if (!outputs) {
        return pup::unexpected<Error>(outputs.error());
    }

    // Expand command with actual outputs for %o substitution.
    // Also capture instruction (after variable expansion, before pattern substitution).
    auto cmd_result = expand_command(ctx, eff_command, flags, *outputs, &instruction_pattern);
    if (!cmd_result) {
        return pup::unexpected<Error>(cmd_result.error());
    }
    cmd_text = *cmd_result;

    if (eff_display) {
        auto disp_result = expand_command(ctx, *eff_display, flags, *outputs);
        if (disp_result) {
            display = *disp_result;
        }
    }

    // Expand non-group order-only inputs. Typed groups (<name>) are already handled
    // by the pre-resolution loop above — their NodeIds are in deferred_group_vec.
    // Expression-based group refs may still slip through expand_inputs (rare).
    auto order_only_file_patterns = Vec<parser::PathPattern> {};
    for (auto const& p : all_order_only) {
        if (!p.is_order_only_group) {
            order_only_file_patterns.push_back(p);
        }
    }
    auto order_only_paths = Vec<StringId> {};
    if (!order_only_file_patterns.empty()) {
        auto order_inputs = expand_inputs(ctx, order_only_file_patterns);
        if (order_inputs) {
            order_only_paths = std::move(*order_inputs);
        }
    }

    auto instr_sv = str(instruction_pattern);
    auto has_group_pattern = instr_sv.find("%<") != std::string_view::npos
        || instr_sv.find("%{") != std::string_view::npos;
    auto final_instruction = is_empty(instruction_pattern) || has_group_pattern
        ? cmd_text
        : instruction_pattern;

    auto cmd_id = create_command_node(ctx, state, str(final_instruction), str(display));
    if (!cmd_id) {
        return pup::unexpected<Error>(cmd_id.error());
    }

    auto cmd_info = CommandInfo {
        .node_id = *cmd_id,
        .command = cmd_text,
        .display = display,
        .inputs = file_inputs,
        .order_only_inputs = order_only_paths,
        .outputs = *outputs,
        .working_dir = intern(str(ctx.current_dir)),
    };

    // Use scanner_registry (new modular approach) if available, fall back to pattern_registry
    auto generated_rules = Vec<GeneratedRule> {};
    if (ctx.options.scanner_registry && !ctx.options.scanner_registry->empty()) {
        generated_rules = ctx.options.scanner_registry->match_and_generate(cmd_info);
    } else if (ctx.options.pattern_registry && !ctx.options.pattern_registry->empty()) {
        generated_rules = ctx.options.pattern_registry->match_and_generate(cmd_info);
    }

    process_generated_rules(ctx, state, generated_rules, *cmd_id, deferred_group_vec);

    // Create edges from inputs to command and collect operand NodeIds
    // Use file_inputs (excludes glob patterns which aren't valid paths)
    // Skip group references - they are handled by deferred edge resolution (order-only)
    auto input_ids = Vec<NodeId> {};
    for (auto input : file_inputs) {
        if (is_order_only_group_reference(str(input))) {
            continue;
        }
        auto input_id = resolve_input_node(ctx, str(input));
        if (!input_id) {
            return pup::unexpected<Error>(input_id.error());
        }
        auto edge_result = add_edge(ctx.state->graph, *input_id, *cmd_id);
        if (!edge_result) {
            return pup::unexpected<Error>(edge_result.error());
        }
        input_ids.push_back(*input_id);
    }

    // Create edges from command to outputs and collect operand NodeIds
    auto output_ids = Vec<NodeId> {};
    for (auto output_path : *outputs) {
        auto output_id = ensure_file_node(ctx.state->graph, output_path, NodeType::Generated);
        if (!output_id) {
            return pup::unexpected<Error>(output_id.error());
        }

        // Check for duplicate output - another command already produces this file
        auto output_inputs = get_inputs(ctx.state->graph, *output_id);
        if (!output_inputs.empty()) {
            for (auto existing_id : output_inputs) {
                if (node_id::is_command(existing_id)) {
                    // Check if this is a phi-node case (complementary guards)
                    // Allow multiple commands producing the same output if they have
                    // mutually exclusive guards (same condition, opposite polarity)
                    auto const* existing_cmd = get_command_node(ctx.state->graph, existing_id);
                    if (existing_cmd && are_guards_mutually_exclusive(existing_cmd->guards, ctx.condition_stack)) {
                        continue;
                    }
                    auto existing_cmd_id = expand_instruction(ctx.state->graph, existing_id, ctx.state->path_cache);
                    auto existing_cmd_sv = global_pool().get(existing_cmd_id);
                    if (existing_cmd_sv.empty()) {
                        existing_cmd_sv = "<unknown>";
                    }
                    auto output_path_sv = get_full_path(ctx.state->graph, *output_id, ctx.state->path_cache);
                    auto err = Buf {};
                    err.fmt("Unable to create output '{}' because it is already owned by command:\n  {}", output_path_sv, existing_cmd_sv);
                    return make_error<void>(ErrorCode::DuplicateNode, err.view());
                }
            }
        }

        auto edge_result = add_edge(ctx.state->graph, *cmd_id, *output_id);
        if (!edge_result) {
            return pup::unexpected<Error>(edge_result.error());
        }
        output_ids.push_back(*output_id);

        // Add to output group {name} if specified
        if (eff_output_group && is_context_active(ctx)) {
            auto gkey = to_underlying(*eff_output_group);
            ctx.groups.get_or_create(gkey).push_back(*output_id);
        }

        // Add to order-only group <name> if specified
        if (eff_output_oo_group && is_context_active(ctx)) {
            auto dir = StringId::Empty;

            if (eff_oo_group_dir) {
                auto expanded = parser::expand(*ctx.eval, *eff_oo_group_dir);
                if (expanded) {
                    dir = normalize_group_dir(str(*expanded), str(ctx.current_dir), str(ctx.options.source_root));
                }
            }

            if (is_empty(dir)) {
                dir = is_empty(ctx.current_dir) ? intern(".") : ctx.current_dir;
            }

            auto group_id_result = get_or_create_group_node(ctx, state, str(dir), str(*eff_output_oo_group));
            if (group_id_result) {
                // Add edge: file → group (file is member of group)
                (void)add_edge(ctx.state->graph, *output_id, *group_id_result, LinkType::Group);
            }
        }
    }

    // Store explicit operands on the command node for expand_instruction()
    if (auto* cmd = get_command_node(ctx.state->graph, *cmd_id)) {
        cmd->inputs = std::move(input_ids);
        cmd->outputs = std::move(output_ids);
    }

    // Create order-only edges from the pre-expanded paths
    // Skip group references (deferred edge creation) and glob patterns (not valid paths)
    for (auto oi : order_only_paths) {
        auto oi_sv = str(oi);
        if (is_order_only_group_reference(oi_sv) || parser::has_glob_chars(oi_sv)) {
            continue;
        }
        auto oi_node = resolve_input_node(ctx, oi_sv);
        if (oi_node) {
            (void)add_edge(ctx.state->graph, *oi_node, *cmd_id, LinkType::OrderOnly);
        }
    }

    add_deferred_group_edges(state, deferred_group_vec, *cmd_id);

    return {};
}

auto process_rule(
    BuilderContext& ctx,
    Builder& state,
    parser::Rule const& rule
) -> Result<void>
{
    // Apply any pending weak assignments (??=) before expanding commands
    // This ensures ??= assignments that precede rules take effect
    apply_pending_weak_assignments(ctx, state);

    // Expand input patterns
    auto inputs = Result<Vec<StringId>> { expand_inputs(ctx, rule.inputs) };
    if (!inputs) {
        return pup::unexpected<Error>(inputs.error());
    }

    // Skip rules where input pattern evaluated to empty (tup behavior)
    // - rule.inputs.empty() means no input pattern was specified (": |> cmd")
    // - inputs->empty() means the pattern(s) evaluated to no files
    // Only skip if pattern was specified but produced nothing
    if (!rule.foreach_ && !rule.inputs.empty() && inputs->empty()) {
        return {};
    }

    if (rule.foreach_) {
        auto patterns = Vec<StringId> {};
        auto files = Vec<StringId> {};
        for (auto inp : *inputs) {
            if (parser::has_glob_chars(str(inp))) {
                patterns.push_back(inp);
            } else {
                files.push_back(inp);
            }
        }

        // Foreach rule: create one command per file, include patterns for %g
        for (auto file : files) {
            auto iter_inputs = patterns;
            iter_inputs.push_back(file);
            auto result = expand_rule(ctx, state, rule, iter_inputs);
            if (!result) {
                return pup::unexpected<Error>(result.error());
            }
        }
    } else {
        // Normal rule: single command for all inputs
        auto result = expand_rule(ctx, state, rule, *inputs);
        if (!result) {
            return pup::unexpected<Error>(result.error());
        }
    }

    return {};
}

auto process_statement(
    BuilderContext& ctx,
    Builder& state,
    parser::Statement const& stmt
) -> Result<void>
{
    if (ctx.eval && ctx.eval->on_statement) {
        auto dir = is_empty(ctx.current_dir) ? std::string_view { "." } : str(ctx.current_dir);
        ctx.eval->on_statement(stmt, dir);
    }

    if (auto const* rule = stmt.as<parser::Rule>()) {
        return process_rule(ctx, state, *rule);
    }

    if (auto const* macro = stmt.as<parser::BangMacro>()) {
        return process_bang_macro(ctx, *macro);
    }

    if (auto const* assign = stmt.as<parser::Assignment>()) {
        return process_assignment(ctx, state, *assign);
    }

    if (auto const* cond = stmt.as<parser::Conditional>()) {
        return process_conditional(ctx, state, *cond);
    }

    if (auto const* inc = stmt.as<parser::Include>()) {
        return process_include(ctx, state, *inc);
    }

    if (auto const* imp = stmt.as<parser::Import>()) {
        return process_import(ctx, state, *imp);
    }

    if (auto const* exp = stmt.as<parser::Export>()) {
        return process_export(ctx, *exp);
    }

    return {};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// §11 — Public API
// ---------------------------------------------------------------------------

auto make_builder(BuilderOptions opts) -> Builder
{
    auto state = Builder {};
    state.options = std::move(opts);
    return state;
}

auto add_tupfile(
    BuildGraph& build_state,
    parser::Tupfile const& tupfile,
    parser::EvalContext& eval,
    Builder& state
) -> Result<void>
{
    // Compute current_dir relative to config_root (where Tupfiles live)
    // In 3-tree builds, config_root differs from source_root, but the directory
    // structure mirrors the source tree, so this relative path is used for both
    // config lookup and source file glob expansion.
    auto const& tupfile_root = is_empty(state.options.config_root)
        ? str(state.options.source_root)
        : str(state.options.config_root);
    auto tupfile_filename_sv = str(tupfile.filename);
    auto tupfile_parent = pup::path::parent(tupfile_filename_sv);
    auto relative_dir_sv = global_pool().get(pup::path::relative(tupfile_parent, tupfile_root));
    auto relative_dir_str = (relative_dir_sv == ".") ? std::string_view {} : relative_dir_sv;

    auto ctx = BuilderContext {
        .state = &build_state,
        .eval = &eval,
        .vars = eval.vars,
        .options = state.options,
        .current_dir = intern(relative_dir_str),
        .current_file = tupfile.filename,
    };

    // Create Tupfile node and add to sticky_sources for dependency tracking
    // For 3-tree builds, store relative to config_root (Tupfile's actual location)
    auto tupfile_rel = global_pool().get(pup::path::relative(tupfile_filename_sv, tupfile_root));
    auto tupfile_path_id = ctx.state->graph.paths.intern_path(tupfile_rel, global_pool(), PathId::SourceRoot);
    auto tupfile_node_result = ensure_file_node(ctx.state->graph, tupfile_path_id, NodeType::File);
    if (tupfile_node_result) {
        ctx.sticky_sources.push_back(*tupfile_node_result);
    }

    // Create Variable nodes for fine-grained config dependency tracking
    // Each config variable becomes a node so commands only depend on variables they use
    // Only create nodes once (first Tupfile); subsequent Tupfiles reuse existing nodes
    if (eval.config_vars && state.config_var_nodes.empty()) {
        // Get config directory for Variable node parent (typically the -B directory)
        auto config_dir_id = NodeId { 0 };
        if (!is_empty(state.options.config_path)) {
            auto config_parent = pup::path::parent(str(state.options.config_path));
            auto config_dir_rel_sv = global_pool().get(pup::path::relative(config_parent, str(state.options.source_root)));
            auto config_dir_rel = (config_dir_rel_sv.empty() || config_dir_rel_sv == ".") ? std::string_view {} : config_dir_rel_sv;
            auto dir_path_id = build_state.graph.paths.intern_path(config_dir_rel, global_pool(), PathId::SourceRoot);
            auto dir_result = ensure_file_node(build_state.graph, dir_path_id, NodeType::Directory);
            if (dir_result) {
                config_dir_id = *dir_result;
            }
        }

        for (auto const& var_name : eval.config_vars->names()) {
            if (var_name.starts_with(parser::builtin_vars::CONFIG_)) {
                continue;
            }

            auto var_name_id = to_underlying(intern(var_name));

            if (auto existing = find_by_dir_name(build_state.graph, config_dir_id, var_name)) {
                state.config_var_nodes.insert(var_name_id, *existing);
                continue;
            }

            auto value = eval.config_vars->get(var_name);
            auto node = FileNode {
                .type = NodeType::Variable,
                .name = intern(var_name),
                .parent_dir = config_dir_id,
                .content_hash = sha256(value),
            };

            auto var_id_result = add_file_node(build_state.graph, std::move(node));
            if (var_id_result) {
                state.config_var_nodes.insert(var_name_id, *var_id_result);
            }
        }
    }

    // Create/find virtual $ directory for imported env vars (like tup's env_dt)
    // Only initialize once; subsequent Tupfiles reuse existing nodes
    if (state.env_var_dir_id == INVALID_NODE_ID) {
        // Check if $ directory already exists in graph (from same build session)
        if (auto existing = find_by_dir_name(build_state.graph, NodeId { 0 }, "$")) {
            state.env_var_dir_id = *existing;
        } else {
            // Create new $ directory under root
            auto env_dir_node = FileNode {
                .type = NodeType::Directory,
                .name = intern("$"),
                .parent_dir = NodeId { 0 },
            };
            auto result = add_file_node(build_state.graph, std::move(env_dir_node));
            if (result) {
                state.env_var_dir_id = *result;
            }
        }
    }

    // Thread string pool into EvalContext for StringId lookups
    eval.string_pool = &global_pool();

    // Set up callback to track which config variables are used during expansion
    eval.on_config_var_used = [&ctx](std::string_view name) {
        ctx.used_config_vars.insert(to_underlying(intern(name)));
    };

    // Set up callback to track which imported env variables are used during expansion.
    // Also ensure an env Variable node exists for any env var that resolves from the
    // environment, so TUP_PLATFORM/TUP_ARCH (auto-resolved, not via `import`) get the
    // same tracking node as explicitly imported vars and fold into command identity.
    eval.imported_vars = &state.imported_var_names;
    eval.on_env_var_used = [&ctx, &state](std::string_view name) {
        ctx.used_env_vars.insert(to_underlying(intern(name)));
        auto name_buf = Buf {};
        name_buf += name;
        if (auto const* env_val = std::getenv(name_buf.c_str())) {
            (void)ensure_env_var_node(ctx, state, name, std::string_view { env_val });
        }
    };

    // Wire up transitive dependency trackers for variable tracking
    eval.var_config_deps = &state.var_config_deps;
    eval.var_env_deps = &state.var_env_deps;

    // Set up resolve_group callback for {group} pattern expansion
    eval.resolve_group = [&ctx](std::string_view name
                         ) -> Vec<StringId> {
        auto gkey = to_underlying(intern(name));
        auto const* members = ctx.groups.find(gkey);
        if (!members) {
            return {};
        }
        auto paths = Vec<StringId> {};
        for (auto id : *members) {
            auto path_sv = get_full_path(ctx.state->graph, id, ctx.state->path_cache);
            if (!path_sv.empty()) {
                paths.push_back(intern(path_sv));
            }
        }
        return paths;
    };

    // Set up resolve_order_only_group callback for %<group> pattern expansion in commands
    // This is for local group references (no directory prefix) - uses current directory
    // Groups are first-class nodes; lookup via graph edges (file → group)
    eval.resolve_order_only_group = [&ctx, &state](std::string_view name
                                    ) -> Vec<StringId> {
        auto dir = is_empty(ctx.current_dir) ? std::string_view { "." } : str(ctx.current_dir);
        auto key_buf = Buf {};
        key_buf += dir;
        key_buf += '/';
        key_buf += name;
        auto key_id = to_underlying(intern(key_buf.view()));
        auto const* node_id = state.group_nodes.find(key_id);
        if (!node_id) {
            return {};
        }
        auto paths = Vec<StringId> {};
        auto members = get_group_members(ctx.state->graph, *node_id);
        for (auto id : members) {
            auto path_sv = get_full_path(ctx.state->graph, id, ctx.state->path_cache);
            if (!path_sv.empty()) {
                paths.push_back(intern(path_sv));
            }
        }
        return paths;
    };

    for (auto const& stmt : tupfile.statements) {
        auto result = process_statement(ctx, state, *stmt);
        if (!result) {
            state.errors.push_back(result.error().message);
            if (!state.options.verbose) {
                return pup::unexpected<Error>(result.error());
            }
        }
    }

    // Apply pending weak assignments (??=) - last wins
    apply_pending_weak_assignments(ctx, state);

    // Copy errors and warnings
    for (auto& err : ctx.errors) {
        state.errors.push_back(std::move(err));
    }
    for (auto& warn : ctx.warnings) {
        state.warnings.push_back(std::move(warn));
    }

    return {};
}

auto finalize_graph(
    BuildGraph& build_state,
    Builder& state
) -> Result<void>
{
    auto& g = build_state.graph;

    // Pass 1: Create graph edges and accumulate members per (command, group_name).
    // Same-named groups from different directories contribute to the same replacement.
    // Key: packed (command_id << 32 | interned_group_name)
    auto pack_key = [](NodeId cmd, std::uint32_t name_id) -> std::uint64_t {
        return (static_cast<std::uint64_t>(cmd) << 32) | name_id;
    };

    auto accumulated = Vec<std::pair<std::uint64_t, Vec<NodeId>>> {};

    for (auto const& edge : state.deferred_edges) {
        auto const* group_node = get_file_node(g, edge.group_id);
        if (!group_node || group_node->type != NodeType::Group) {
            continue;
        }

        auto members = get_group_members(g, edge.group_id);
        if (members.empty()) {
            auto group_path_sv = get_full_path(g, edge.group_id, build_state.path_cache);
            auto warn = Buf {};
            warn.fmt("order-only group {} has no members", group_path_sv);
            state.warnings.push_back(warn.intern(global_pool()));
            continue;
        }

        (void)add_edge(g, edge.group_id, edge.command_id, LinkType::OrderOnly);

        auto group_basename = str(group_node->name);
        if (group_basename.size() > 2 && group_basename.front() == '<' && group_basename.back() == '>') {
            auto bare_name = group_basename.substr(1, group_basename.size() - 2);
            auto name_id = to_underlying(intern(bare_name));
            auto key = pack_key(edge.command_id, name_id);
            auto it = std::lower_bound(accumulated.begin(), accumulated.end(), key, [](auto const& p, auto k) { return p.first < k; });
            if (it != accumulated.end() && it->first == key) {
                it->second.insert(it->second.end(), members.begin(), members.end());
            } else {
                accumulated.insert(it, { key, std::move(members) });
            }
        }
    }

    // Pass 2: Replace %<group> patterns with the full accumulated member lists.
    for (auto const& [key, members] : accumulated) {
        auto command_id = static_cast<NodeId>(key >> 32);
        auto name_id = static_cast<StringId>(key & 0xFFFFFFFF);
        auto group_name = str(name_id);
        auto pat_buf = Buf {};
        pat_buf += "%<";
        pat_buf += group_name;
        pat_buf += '>';
        auto pattern_sv = pat_buf.view();

        auto* cmd_node = get_command_node(g, command_id);
        if (!cmd_node) {
            continue;
        }
        auto cmd_sv = str(cmd_node->instruction_id);
        if (cmd_sv.find(pattern_sv) == std::string_view::npos) {
            continue;
        }

        auto source_dir_sv = str(cmd_node->source_dir);
        auto canonical_cwd = StringId::Empty;
        if (!is_empty(state.options.source_root) && !is_empty(state.options.output_root)
            && str(state.options.source_root) != str(state.options.output_root)) {
            auto joined_sv = global_pool().get(pup::path::join(str(state.options.source_root), source_dir_sv));
            auto r = pup::platform::canonical(joined_sv);
            if (r) {
                canonical_cwd = *r;
            }
        }
        auto tc = PathTransformContext {
            .source_to_root = pup::compute_source_to_root(source_dir_sv),
            .current_dir_id = intern(source_dir_sv),
            .source_root = state.options.source_root,
            .config_root = state.options.config_root,
            .output_root = state.options.output_root,
            .canonical_cwd = canonical_cwd,
        };

        auto replacement = Buf {};
        for (auto id : members) {
            auto p_sv = get_full_path(g, id, build_state.path_cache);
            if (!p_sv.empty()) {
                if (!replacement.empty()) {
                    replacement += ' ';
                }
                replacement += str(transform_output_path(tc, p_sv));
            }
        }

        {
            auto result = Buf {};
            auto pos = cmd_sv.find(pattern_sv);
            std::size_t last = 0;
            while (pos != std::string_view::npos) {
                result += cmd_sv.substr(last, pos - last);
                result += replacement.view();
                last = pos + pattern_sv.size();
                pos = cmd_sv.find(pattern_sv, last);
            }
            result += cmd_sv.substr(last);
            cmd_node->instruction_id = intern(result.view());
        }

        auto display_sv = str(cmd_node->display);
        if (display_sv.find(pattern_sv) != std::string_view::npos) {
            auto result = Buf {};
            auto pos = display_sv.find(pattern_sv);
            std::size_t last = 0;
            while (pos != std::string_view::npos) {
                result += display_sv.substr(last, pos - last);
                result += replacement.view();
                last = pos + pattern_sv.size();
                pos = display_sv.find(pattern_sv, last);
            }
            result += display_sv.substr(last);
            cmd_node->display = intern(result.view());
        }
    }

    state.deferred_edges.clear();
    return {};
}

} // namespace pup::graph
