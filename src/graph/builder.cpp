// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/builder.hpp"
#include "pup/core/hash.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/rule_pattern.hpp"
#include "pup/parser/eval.hpp"
#include "pup/parser/glob.hpp"
#include "pup/parser/parser.hpp"

#include <cstdio>
#include <format>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace pup::graph {

namespace fs = std::filesystem;

namespace {

/// Strip trailing slashes from a path string
auto strip_trailing_slashes(std::string str) -> std::string
{
    while (!str.empty() && (str.back() == '/' || str.back() == '\\')) {
        str.pop_back();
    }
    return str;
}

/// Normalize a file path for consistent lookup
/// - Removes double slashes
/// - Resolves . and .. components using lexically_normal
auto normalize_path(std::string const& path_str) -> std::string
{
    if (path_str.empty()) {
        return path_str;
    }
    auto path = fs::path { path_str }.lexically_normal();
    return path.string();
}

/// Normalize a directory path for group key lookup.
/// - Strips trailing slashes
/// - Converts absolute paths to project-relative
/// - Resolves parent references (..) against current_dir
auto normalize_group_dir(
    std::string const& path_str,
    fs::path const& current_dir,
    fs::path const& source_root
) -> std::string
{
    auto cleaned = strip_trailing_slashes(path_str);
    auto path = fs::path { cleaned }.lexically_normal();

    if (path.is_absolute()) {
        path = fs::relative(path, source_root);
    } else if (!current_dir.empty() && !path.empty()) {
        // Only combine with current_dir if path needs parent resolution (starts with ..)
        // Paths like $(ROOT)/foo expand to root-relative and should NOT be combined
        auto first = *path.begin();
        if (first == "..") {
            path = (current_dir / path).lexically_normal();
        }
    }

    return path.empty() ? "." : path.string();
}

/// Parsed group reference from a path like "../include/<gen-headers>"
struct GroupReference {
    std::string group_name;
    std::string group_dir;
};

/// Check if a path is an order-only group reference (ends with <name>)
/// Examples: "<group>", "path/<group>", "../include/<gen-headers>"
auto is_order_only_group_reference(std::string_view path) -> bool
{
    auto lt_pos = path.rfind('<');
    return lt_pos != std::string_view::npos && !path.empty() && path.back() == '>';
}

/// Trigger demand-driven parsing for a directory if it contains a Tupfile.
/// This is used when referencing cross-directory groups or generated files.
auto request_demand_driven_parse(
    parser::EvalContext const& eval,
    fs::path const& dir_path
) -> void
{
    if (eval.request_directory && eval.available_tupfile_dirs) {
        if (eval.available_tupfile_dirs->contains(dir_path)) {
            (void)eval.request_directory(dir_path);
        }
    }
}

/// Parse a group reference from a path expression
/// Returns nullopt if the path doesn't contain a valid <group> suffix
auto parse_group_reference(
    std::string const& path,
    fs::path const& current_dir,
    fs::path const& source_root
) -> std::optional<GroupReference>
{
    auto lt_pos = path.rfind('<');
    auto gt_pos = path.rfind('>');
    if (lt_pos == std::string::npos || gt_pos == std::string::npos
        || gt_pos != path.size() - 1 || gt_pos <= lt_pos) {
        return std::nullopt;
    }
    auto group_name = path.substr(lt_pos + 1, gt_pos - lt_pos - 1);
    if (group_name.empty()) {
        return std::nullopt;
    }
    auto dir_part = path.substr(0, lt_pos);
    auto group_dir = normalize_group_dir(dir_part, current_dir, source_root);
    return GroupReference { std::move(group_name), std::move(group_dir) };
}

/// Transform a project-root-relative path to be relative to source directory
/// Used for command expansion where commands run from Tupfile directory
auto make_source_relative(
    std::string const& path,
    std::string_view source_to_root,
    std::string_view current_dir_str
) -> std::string
{
    if (path.empty() || path[0] == '/') {
        return path;
    }
    // Paths starting with .. are relative to source_root.
    // If we're in a subdirectory (current_dir_str not empty), prepend
    // source_to_root to adjust the path for the working directory.
    if (path.size() >= 2 && path[0] == '.' && path[1] == '.') {
        if (!source_to_root.empty() && !current_dir_str.empty()) {
            return std::string { source_to_root } + path;
        }
        return path;
    }
    if (source_to_root.empty()) {
        return path;
    }
    if (!current_dir_str.empty() && path.starts_with(std::string { current_dir_str } + "/")) {
        return path.substr(current_dir_str.size() + 1);
    }
    if (!current_dir_str.empty() && path == current_dir_str) {
        return ".";
    }
    return std::string { source_to_root } + path;
}

/// Compute the "../" prefix needed to go from current_dir to project root
auto compute_source_to_root(fs::path const& current_dir) -> std::string
{
    auto result = std::string {};
    for (auto const& comp : current_dir) {
        auto s = comp.string();
        if (s != "." && s != "/" && !s.empty()) {
            result += "../";
        }
    }
    return result;
}

/// Strip the build root prefix from a path if present.
/// E.g., "build/src/main.o" → "src/main.o" when build_root_name is "build"
auto strip_build_prefix(
    std::string_view path,
    std::string_view build_root_name
) -> std::string
{
    if (build_root_name.empty()) {
        return std::string { path };
    }
    auto prefix_len = build_root_name.size() + 1; // includes "/"
    if (path.size() > prefix_len
        && path.substr(0, build_root_name.size()) == build_root_name
        && path[build_root_name.size()] == '/') {
        return std::string { path.substr(prefix_len) };
    }
    return std::string { path };
}

/// Context for transforming paths to Tupfile-relative coordinates.
///
/// Commands execute from the Tupfile's source directory, so all paths in commands
/// must be relative to that directory. With node traversal:
/// - Outputs are already variant-mapped (e.g., "build/src/foo.o")
/// - Inputs are source-root-relative (e.g., "src/lib/bar.c")
/// The transform functions below convert both to Tupfile-relative paths.
struct PathTransformContext {
    std::string source_to_root;
    std::string current_dir_str;
    fs::path source_root;
    fs::path output_root;
};

auto make_transform_context(BuilderContext const& ctx) -> PathTransformContext
{
    return PathTransformContext {
        .source_to_root = compute_source_to_root(ctx.current_dir),
        .current_dir_str = ctx.current_dir.string(),
        .source_root = ctx.options.source_root,
        .output_root = ctx.options.output_root,
    };
}

/// Transform an input path to Tupfile-relative for command expansion.
/// Input paths are source-relative. For Generated/Ghost files under BUILD_ROOT_ID,
/// we need to use get_full_path() to get the path including the build root prefix.
auto transform_input_path(
    BuildGraph& graph,
    PathTransformContext const& tc,
    std::string const& inp
) -> std::string
{
    // Check if this input refers to a Generated/Ghost file under BUILD_ROOT_ID
    // If so, its full path includes the build root prefix (e.g., "build/include/header.h")
    if (auto node_id = graph.find_by_path(inp, BUILD_ROOT_ID)) {
        auto full_path = graph.get_full_path(*node_id);
        if (!full_path.empty()) {
            return make_source_relative(full_path, tc.source_to_root, tc.current_dir_str);
        }
    }

    // Node may not exist yet (transform happens before resolve_input_node creates it).
    // Check if file exists in build directory - if so, use variant-prefixed path.
    auto build_root_name = std::string { graph.get_build_root_name() };
    if (!build_root_name.empty()) {
        auto build_path = tc.output_root / inp;
        if (fs::exists(build_path)) {
            auto full_path = build_root_name + "/" + inp;
            return make_source_relative(full_path, tc.source_to_root, tc.current_dir_str);
        }
    }

    // Source file or not found - use path as-is
    return make_source_relative(inp, tc.source_to_root, tc.current_dir_str);
}

/// Transform an output path to Tupfile-relative for command expansion.
/// Outputs are already stored at variant-mapped paths (e.g., "build/src/main.o").
/// This function just converts to Tupfile-relative (e.g., "../../build/src/main.o").
auto transform_output_path(
    PathTransformContext const& tc,
    std::string const& out
) -> std::string
{
    // Outputs are already variant-mapped (stored under BUILD_ROOT_ID by expand_outputs).
    // Just make the path relative to the Tupfile directory.
    return make_source_relative(out, tc.source_to_root, tc.current_dir_str);
}

/// Get all files that are members of a group (via file → group edges)
/// Returns file NodeIds by finding all input edges to the group node
auto get_group_members(BuildGraph& graph, NodeId group_id) -> std::vector<NodeId>
{
    // Files point TO groups via Group edges (file → group)
    // So group.inputs contains the member files
    return graph.get_inputs(group_id);
}

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

/// Walk a path from a starting directory node, creating intermediate directories.
/// Returns the final directory node.
///
/// @param graph The build graph
/// @param start_dir_id Starting directory node (typically Tupfile's parent)
/// @param path Path to walk (already variable-expanded)
/// @return NodeId of the final directory
auto walk_path_to_directory(
    BuildGraph& graph,
    NodeId start_dir_id,
    std::string_view path
) -> NodeId
{
    if (path.empty() || path == ".") {
        return start_dir_id;
    }

    auto current_id = start_dir_id;
    auto p = fs::path { path };

    for (auto const& component : p) {
        auto comp_str = component.string();
        if (comp_str.empty() || comp_str == ".") {
            continue;
        }

        if (comp_str == "..") {
            // Walk to parent - node's parent_dir points to parent (0 = root)
            // Only go up if we're not already at root
            if (current_id != NodeId { 0 }) {
                auto* node = graph.get_file_node(current_id);
                if (node) {
                    current_id = node->parent_dir;
                }
            }
            // If already at root (current_id == 0), stay at root
        } else {
            // Find or create child directory
            if (auto child = graph.find_by_dir_name(current_id, comp_str)) {
                current_id = *child;
            } else {
                // Create new directory node
                auto node = FileNode {
                    .type = NodeType::Directory,
                    .name = graph.intern(comp_str),
                    .parent_dir = current_id,
                };
                auto result = graph.add_file_node(std::move(node));
                if (result) {
                    current_id = *result;
                }
            }
        }
    }

    return current_id;
}

/// Walk a path and create/find the file node at the end.
/// @param graph The build graph
/// @param start_id Starting node (BUILD_ROOT_ID for generated, SOURCE_ROOT_ID for source)
/// @param path Path to the file (relative to start_id)
/// @param type NodeType for the file node
/// @return NodeId of the file node
auto walk_to_file_node(
    BuildGraph& graph,
    NodeId start_id,
    std::string_view path,
    NodeType type
) -> Result<NodeId>
{
    if (path.empty()) {
        return make_error<NodeId>(ErrorCode::InvalidArgument, "Empty path");
    }

    // Walk the path components
    auto p = fs::path { path };
    auto parent_path = p.parent_path();
    auto basename = p.filename().string();

    // Walk to parent directory
    auto target_dir_id = start_id;
    if (!parent_path.empty() && parent_path != ".") {
        target_dir_id = walk_path_to_directory(graph, start_id, parent_path.string());
    }

    // Find or create the file node
    if (auto existing = graph.find_by_dir_name(target_dir_id, basename)) {
        // Handle type upgrade (Ghost → Generated, File → Generated)
        if (type == NodeType::Generated) {
            auto* node = graph.get_file_node(*existing);
            if (node && (node->type == NodeType::Ghost || node->type == NodeType::File)) {
                node->type = NodeType::Generated;
            }
        }
        return *existing;
    }

    // Create new node
    auto node = FileNode {
        .type = type,
        .name = graph.intern(basename),
        .parent_dir = target_dir_id,
    };
    return graph.add_file_node(std::move(node));
}

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

constexpr auto MAX_DIRECTORY_DEPTH = 128;

// ============================================================================
// Forward declarations for internal free functions
// ============================================================================

auto process_statement(
    BuilderContext& ctx,
    BuilderState& state,
    parser::Statement const& stmt
) -> Result<void>;

auto process_rule(
    BuilderContext& ctx,
    BuilderState& state,
    parser::Rule const& rule
) -> Result<void>;

auto process_bang_macro(
    BuilderContext& ctx,
    parser::BangMacro const& macro
) -> Result<void>;

auto process_assignment(
    BuilderContext& ctx,
    BuilderState& state,
    parser::Assignment const& assign
) -> Result<void>;

auto process_conditional(
    BuilderContext& ctx,
    BuilderState& state,
    parser::Conditional const& cond
) -> Result<void>;

auto process_include(
    BuilderContext& ctx,
    BuilderState& state,
    parser::Include const& inc
) -> Result<void>;

auto process_import(
    BuilderContext& ctx,
    BuilderState& state,
    parser::Import const& imp
) -> Result<void>;

auto process_export(
    BuilderContext& ctx,
    parser::Export const& exp
) -> Result<void>;

auto expand_rule(
    BuilderContext& ctx,
    BuilderState& state,
    parser::Rule const& rule,
    std::vector<std::string> const& inputs
) -> Result<void>;

auto expand_inputs(
    BuilderContext& ctx,
    std::vector<parser::PathPattern> const& patterns
) -> Result<std::vector<std::string>>;

auto expand_outputs(
    BuilderContext& ctx,
    std::vector<parser::PathPattern> const& patterns,
    parser::PatternFlags const& flags
) -> Result<std::vector<std::string>>;

auto expand_command(
    BuilderContext& ctx,
    parser::Expression const& cmd,
    parser::PatternFlags flags,
    std::vector<std::string> const& outputs
) -> Result<std::string>;

auto get_or_create_directory_node(
    BuilderContext& ctx,
    std::filesystem::path const& dir_path,
    int depth = 0
) -> Result<NodeId>;

auto get_or_create_file_node(
    BuilderContext& ctx,
    std::string const& path,
    NodeType type = NodeType::File
) -> Result<NodeId>;

auto resolve_input_node(
    BuilderContext& ctx,
    std::string const& path
) -> Result<NodeId>;

auto get_or_create_group_node(
    BuilderContext& ctx,
    BuilderState& state,
    std::string const& directory,
    std::string const& name
) -> Result<NodeId>;

auto create_command_node(
    BuilderContext& ctx,
    BuilderState& state,
    std::string const& command,
    std::string const& display
) -> Result<NodeId>;

/// Search up the directory tree for Tuprules.tup
/// Returns empty path if not found
auto find_tuprules_file(
    fs::path const& start_dir,
    fs::path const& root
) -> fs::path
{
    auto search_dir = start_dir;

    while (search_dir >= root) {
        auto tuprules = fs::path { search_dir / "Tuprules.tup" };
        if (fs::exists(tuprules)) {
            return tuprules;
        }
        if (search_dir == root) {
            break;
        }
        search_dir = search_dir.parent_path();
    }

    return {};
}

/// Resolve an explicit include path (not include_rules)
/// Returns the resolved path or an error
auto resolve_include_path(
    BuilderContext& ctx,
    fs::path const& include_root,
    parser::Expression const& path_expr
) -> Result<fs::path>
{
    auto path_result = parser::expand(*ctx.eval, path_expr);
    if (!path_result) {
        return pup::unexpected<Error>(path_result.error());
    }

    auto resolved = fs::path { include_root / ctx.current_dir / *path_result };
    if (!fs::exists(resolved)) {
        return make_error<fs::path>(ErrorCode::IncludeNotFound, "Include file not found: " + *path_result);
    }
    return resolved;
}

/// Expand a glob pattern against filesystem and graph nodes.
/// Adds matched paths to result vector.
auto expand_glob_pattern(
    BuilderContext& ctx,
    std::string const& path,
    std::vector<std::string>& result
) -> void
{
    auto base = fs::path { ctx.current_dir.empty() ? ctx.options.source_root
                                                   : ctx.options.source_root / ctx.current_dir };

    // First try expanding against filesystem
    auto expanded = parser::glob_expand(path, base);
    if (expanded && !expanded->empty()) {
        for (auto& p : *expanded) {
            // Prefix with current_dir to make path relative to project root
            if (!ctx.current_dir.empty()) {
                result.push_back((ctx.current_dir / p).string());
            } else {
                result.push_back(std::move(p));
            }
        }
        return;
    }

    // No files on disk - look for matching Generated nodes in graph
    // First, try demand-driven parsing of the directory containing the glob pattern
    auto pattern_dir = fs::path { path }.parent_path();
    auto abs_pattern_dir = (ctx.current_dir / pattern_dir).lexically_normal();
    request_demand_driven_parse(*ctx.eval, abs_pattern_dir);

    // Match glob pattern against Generated nodes
    // In 3-tree builds, Generated nodes are stored with build root prefix (e.g., ../build/hello.o)
    // but the glob pattern is relative to current directory (e.g., *.o)
    // We need to strip the build root prefix and match against the relative path
    auto pattern_path = ctx.current_dir.empty() ? path : (ctx.current_dir / path).lexically_normal().string();
    auto glob = parser::Glob { pattern_path };
    auto build_root_name = ctx.graph->get_build_root_name();
    for (auto id : ctx.graph->nodes_of_type(NodeType::Generated)) {
        auto node_path = ctx.graph->get_full_path(id);
        if (node_path.empty()) {
            continue;
        }
        // Strip build root prefix to get source-relative path for matching
        auto match_path = strip_build_prefix(node_path, build_root_name);
        if (glob.matches(match_path)) {
            result.push_back(std::move(node_path));
        }
    }
}

/// Apply exclusion patterns to filter out paths from the result.
/// Handles both glob and non-glob exclusions.
auto apply_exclusions(
    BuilderContext& ctx,
    std::vector<parser::PathPattern> const& patterns,
    std::vector<std::string>& result
) -> void
{
    for (auto const& pattern : patterns) {
        if (!pattern.is_exclusion && !pattern.is_output_exclusion) {
            continue;
        }

        auto paths = parser::expand_path(*ctx.eval, pattern);
        if (!paths) {
            continue;
        }

        for (auto const& excl : *paths) {
            if (ctx.options.expand_globs && parser::has_glob_chars(excl)) {
                auto base = fs::path { ctx.current_dir.empty() ? ctx.options.source_root
                                                               : ctx.options.source_root / ctx.current_dir };
                auto expanded = parser::glob_expand(excl, base);
                if (expanded && !expanded->empty()) {
                    for (auto const& p : *expanded) {
                        auto normalized = ctx.current_dir.empty()
                            ? fs::path { p }.lexically_normal().string()
                            : (ctx.current_dir / p).lexically_normal().string();
                        std::erase(result, normalized);
                    }
                }
            } else {
                auto normalized_excl = ctx.current_dir.empty()
                    ? fs::path { excl }.lexically_normal().string()
                    : (ctx.current_dir / excl).lexically_normal().string();
                std::erase(result, normalized_excl);
            }
        }
    }
}

/// Process generated rules (e.g., DEP commands for dependency scanning).
/// Creates command nodes and edges for each generated rule.
auto process_generated_rules(
    BuilderContext& ctx,
    BuilderState& state,
    std::vector<GeneratedRule> const& generated_rules,
    NodeId parent_cmd_id
) -> void
{
    for (auto const& gen_rule : generated_rules) {
        auto gen_cmd_id = create_command_node(ctx, state, gen_rule.command, gen_rule.display);
        if (!gen_cmd_id) {
            continue;
        }

        // Create edges from inputs to generated command
        for (auto const& input : gen_rule.inputs) {
            auto input_id = resolve_input_node(ctx, input);
            if (input_id) {
                (void)ctx.graph->add_edge(*input_id, *gen_cmd_id);
            }
        }

        // Create order-only edges for generated command (e.g., gen-headers)
        // For group references, defer to resolve_deferred_order_only_edges()
        for (auto const& oi : gen_rule.order_only_inputs) {
            auto group_ref = parse_group_reference(oi, ctx.current_dir, ctx.options.source_root);
            if (group_ref) {
                // This is a group reference - get/create group node and defer edge
                auto group_id_result = get_or_create_group_node(ctx, state, group_ref->group_dir, group_ref->group_name);
                if (group_id_result) {
                    state.deferred_edges.insert({ *group_id_result, *gen_cmd_id });
                }
            } else if (!parser::has_glob_chars(oi)) {
                // Regular file path - create edge directly (skip glob patterns)
                auto oi_id = resolve_input_node(ctx, oi);
                if (oi_id) {
                    (void)ctx.graph->add_order_only_edge(*oi_id, *gen_cmd_id);
                }
            }
        }

        // Add edge from generated command to parent command (dep-scan runs before compile)
        (void)ctx.graph->add_edge(*gen_cmd_id, parent_cmd_id);

        // Store generated rule info on the node for scheduler to handle
        if (auto* node = ctx.graph->get_command_node(*gen_cmd_id)) {
            node->generated_output = gen_rule.outputs.empty() ? GeneratedOutput {} : gen_rule.outputs[0];
            node->output_action = gen_rule.action;
            node->parent_command = gen_rule.parent_command;
        }
    }
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

    auto cmd_str = std::string { *expanded_cmd };
    // Trim leading whitespace
    while (!cmd_str.empty() && (cmd_str.front() == ' ' || cmd_str.front() == '\t')) {
        cmd_str.erase(0, 1);
    }

    if (cmd_str.empty() || cmd_str[0] != '!') {
        return nullptr;
    }

    // Bang macro reference - extract just the macro name (first word after !)
    auto name_end = cmd_str.find_first_of(" \t", 1);
    auto macro_name = (name_end == std::string::npos)
        ? cmd_str.substr(1)
        : cmd_str.substr(1, name_end - 1);

    auto it = ctx.macros.find(macro_name);
    if (it == ctx.macros.end()) {
        return make_error<BangMacroDef const*>(ErrorCode::UnknownMacro, "Unknown bang macro: !" + macro_name);
    }

    return &it->second;
}

// ============================================================================
// Internal free function implementations
// ============================================================================

auto process_statement(
    BuilderContext& ctx,
    BuilderState& state,
    parser::Statement const& stmt
) -> Result<void>
{
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

/// Apply pending weak assignments (??=) - last wins for each variable name
/// Iterates in reverse order so later assignments take precedence
auto apply_pending_weak_assignments(BuilderContext& ctx, BuilderState& state) -> void
{
    if (!ctx.vars || ctx.pending_weak_assignments.empty()) {
        return;
    }
    for (auto it = ctx.pending_weak_assignments.rbegin();
         it != ctx.pending_weak_assignments.rend();
         ++it) {
        if (!ctx.vars->contains(it->name)) {
            ctx.vars->set(it->name, it->value);
            // Record transitive dependencies for this effective assignment
            if (!it->config_deps.empty()) {
                state.var_config_deps[it->name] = std::move(it->config_deps);
            }
            if (!it->env_deps.empty()) {
                state.var_env_deps[it->name] = std::move(it->env_deps);
            }
        }
    }
    ctx.pending_weak_assignments.clear();
}

auto process_rule(
    BuilderContext& ctx,
    BuilderState& state,
    parser::Rule const& rule
) -> Result<void>
{
    // Apply any pending weak assignments (??=) before expanding commands
    // This ensures ??= assignments that precede rules take effect
    apply_pending_weak_assignments(ctx, state);

    // Expand input patterns
    auto inputs = Result<std::vector<std::string>> { expand_inputs(ctx, rule.inputs) };
    if (!inputs) {
        return pup::unexpected<Error>(inputs.error());
    }

    if (rule.foreach_) {
        // Separate glob patterns from files
        // expand_inputs() now returns [pattern, file1, file2, ...] for globs
        auto patterns = std::vector<std::string> {};
        auto files = std::vector<std::string> {};
        for (auto const& inp : *inputs) {
            if (parser::has_glob_chars(inp)) {
                patterns.push_back(inp);
            } else {
                files.push_back(inp);
            }
        }

        // Foreach rule: create one command per file, include patterns for %g
        for (auto const& file : files) {
            auto iter_inputs = patterns;
            iter_inputs.push_back(file);
            auto result = Result<void> { expand_rule(ctx, state, rule, iter_inputs) };
            if (!result) {
                return pup::unexpected<Error>(result.error());
            }
        }
    } else {
        // Normal rule: single command for all inputs
        auto result = Result<void> { expand_rule(ctx, state, rule, *inputs) };
        if (!result) {
            return pup::unexpected<Error>(result.error());
        }
    }

    return {};
}

auto process_bang_macro(
    BuilderContext& ctx,
    parser::BangMacro const& macro
) -> Result<void>
{
    // Store macro definition for later use
    ctx.macros[macro.name] = BangMacroDef {
        .name = macro.name,
        .foreach_ = macro.foreach_,
        .order_only_inputs = macro.order_only_inputs,
        .command = macro.command,
        .display = macro.display,
        .outputs = macro.outputs,
        .extra_outputs = macro.extra_outputs,
        .output_group = macro.output_group,
        .output_order_only_group = macro.output_order_only_group,
        .output_order_only_group_dir = macro.output_order_only_group_dir,
    };

    return {};
}

auto process_assignment(
    BuilderContext& ctx,
    BuilderState& state,
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
    auto saved_config_vars = std::move(ctx.used_config_vars);
    auto saved_env_vars = std::move(ctx.used_env_vars);
    ctx.used_config_vars.clear();
    ctx.used_env_vars.clear();

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

    auto value_before = std::string { db->get(*name) };
    auto is_effective = true;

    // Helper to record transitive dependencies for this variable
    auto record_deps = [&]() {
        if (!captured_config_deps.empty()) {
            auto& deps = state.var_config_deps[*name];
            if (assign.op == parser::Assignment::Op::Set
                || assign.op == parser::Assignment::Op::Define
                || assign.op == parser::Assignment::Op::SoftSet) {
                deps = std::move(captured_config_deps);
            } else if (assign.op == parser::Assignment::Op::Append) {
                deps.merge(captured_config_deps);
            }
        }
        if (!captured_env_deps.empty()) {
            auto& deps = state.var_env_deps[*name];
            if (assign.op == parser::Assignment::Op::Set
                || assign.op == parser::Assignment::Op::Define
                || assign.op == parser::Assignment::Op::SoftSet) {
                deps = std::move(captured_env_deps);
            } else if (assign.op == parser::Assignment::Op::Append) {
                deps.merge(captured_env_deps);
            }
        }
    };

    switch (assign.op) {
    case parser::Assignment::Op::Set:
        db->set(*name, *value);
        record_deps();
        break;
    case parser::Assignment::Op::Append:
        db->append(*name, *value);
        record_deps();
        break;
    case parser::Assignment::Op::Define:
        db->set(*name, *value);
        record_deps();
        break;
    case parser::Assignment::Op::SoftSet:
        // ?= - set only if variable is not already defined (first wins)
        // Only record deps if assignment is effective
        if (!db->contains(*name)) {
            db->set(*name, *value);
            record_deps();
        } else {
            is_effective = false;
        }
        break;
    case parser::Assignment::Op::WeakSet:
        // ??= - deferred assignment, applied before rules (last wins)
        // Store deps with the pending assignment; they'll be recorded when applied
        ctx.pending_weak_assignments.push_back(PendingWeakAssignment {
            .name = *name,
            .value = *value,
            .config_deps = std::move(captured_config_deps),
            .env_deps = std::move(captured_env_deps),
        });
        break;
    }

    if (ctx.eval->on_var_assigned) {
        auto value_after = std::string { db->get(*name) };
        ctx.eval->on_var_assigned(
            *name,
            assign.op,
            value_before,
            value_after,
            ctx.current_file.string(),
            assign.location.line,
            assign.location.column,
            is_effective
        );
    }

    return {};
}

auto process_conditional(
    BuilderContext& ctx,
    BuilderState& state,
    parser::Conditional const& cond
) -> Result<void>
{
    auto condition_true = parser::evaluate_condition(*ctx.eval, cond);

    auto const& body = condition_true ? cond.then_body : cond.else_body;

    for (auto const& stmt : body) {
        auto result = Result<void> { process_statement(ctx, state, *stmt) };
        if (!result) {
            return pup::unexpected<Error>(result.error());
        }
    }

    return {};
}

auto process_include(
    BuilderContext& ctx,
    BuilderState& state,
    parser::Include const& inc
) -> Result<void>
{
    // Include files (Tuprules.tup, etc.) live in config_root (same as Tupfiles)
    // Use config_root if set, otherwise fall back to source_root for traditional builds
    auto const& include_root = ctx.options.config_root.empty() ? ctx.options.source_root : ctx.options.config_root;

    // Find the include file path
    auto include_path = std::string {};
    if (inc.is_rules) {
        auto tuprules = find_tuprules_file(include_root / ctx.current_dir, include_root);
        if (tuprules.empty()) {
            return {}; // No Tuprules.tup found, silently continue
        }
        include_path = tuprules.string();
    } else {
        auto resolved = resolve_include_path(ctx, include_root, inc.path);
        if (!resolved) {
            return pup::unexpected<Error>(resolved.error());
        }
        include_path = resolved->string();
    }

    // Prevent infinite recursion
    if (ctx.included_files.contains(include_path)) {
        return {};
    }
    ctx.included_files.insert(include_path);

    // Add included file to sticky_sources for dependency tracking
    // Included files live in config_root, so use include_root for relative path
    auto inc_rel = fs::relative(include_path, include_root).string();
    auto inc_node_result = get_or_create_file_node(ctx, inc_rel, NodeType::File);
    if (inc_node_result) {
        ctx.sticky_sources.push_back(*inc_node_result);
    }

    // Read the include file
    auto file = std::ifstream { include_path };
    if (!file) {
        return make_error<void>(ErrorCode::IoError, "Cannot open include file: " + include_path);
    }

    auto ss = std::stringstream {};
    ss << file.rdbuf();
    auto source = std::string { ss.str() };

    // Parse the include file
    auto parse_result = parser::parse_tupfile(source, include_path);
    if (!parse_result.success()) {
        for (auto const& err : parse_result.errors) {
            fprintf(stderr, "%s:%d:%d: error: %s\n", include_path.c_str(), err.location.line, err.location.column, err.message.c_str());
        }
        return make_error<void>(ErrorCode::ParseError, "Parse error in include file: " + include_path);
    }

    // For include_rules, temporarily set TUP_CWD to the relative path from
    // the Tupfile directory back to the Tuprules.tup directory. This allows
    // patterns like ROOT = $(TUP_CWD) to work correctly.
    auto old_tup_cwd = std::string {};
    if (inc.is_rules && ctx.eval) {
        old_tup_cwd = ctx.eval->tup_cwd;
        // Compute relative path from Tupfile directory to include file's directory
        auto include_dir = fs::path { include_path }.parent_path();
        auto rel_path = fs::relative(include_dir, include_root / ctx.current_dir);
        ctx.eval->tup_cwd = rel_path.empty() ? "." : rel_path.string();
    }

    // Save and update current_file for variable tracking callback
    auto old_current_file = ctx.current_file;
    ctx.current_file = include_path;

    // Process statements from the included file
    for (auto const& stmt : parse_result.tupfile.statements) {
        auto result = Result<void> { process_statement(ctx, state, *stmt) };
        if (!result) {
            ctx.current_file = old_current_file;
            if (inc.is_rules && ctx.eval) {
                ctx.eval->tup_cwd = old_tup_cwd;
            }
            return pup::unexpected<Error>(result.error());
        }
    }

    // Restore original current_file and TUP_CWD
    ctx.current_file = old_current_file;
    if (inc.is_rules && ctx.eval) {
        ctx.eval->tup_cwd = old_tup_cwd;
    }

    return {};
}

auto process_import(
    BuilderContext& ctx,
    BuilderState& state,
    parser::Import const& imp
) -> Result<void>
{
    // Per tup manual: "sets a variable inside the Tupfile that has the value
    // of the environment variable"
    auto value = std::string {};

    // 1. Try environment first
    if (auto const* env_val = std::getenv(imp.var_name.c_str())) {
        value = env_val;
    }
    // 2. Fall back to cached value from previous build (passed via options)
    else if (auto it = state.options.cached_env_vars.find(imp.var_name);
             it != state.options.cached_env_vars.end()) {
        value = it->second;
    }
    // 3. Fall back to default value
    else if (imp.default_value) {
        auto expanded = parser::expand(*ctx.eval, *imp.default_value);
        if (!expanded) {
            return pup::unexpected<Error>(expanded.error());
        }
        value = *expanded;
    }
    // If no env, no cache, and no default, variable remains empty (tup behavior)

    // Create/update Variable node under $ directory for persistence
    if (state.env_var_dir_id != INVALID_NODE_ID) {
        auto node_name = imp.var_name + "=" + value;
        auto content_hash = sha256(value);

        // Check if we already have a node for this variable (from same build session)
        auto it = state.imported_env_var_nodes.find(imp.var_name);
        auto const name_id = ctx.graph->intern(node_name);
        if (it != state.imported_env_var_nodes.end()) {
            // Update existing node in-place if value changed
            auto* existing = ctx.graph->get_file_node(it->second);
            if (existing && existing->name != name_id) {
                existing->name = name_id;
                existing->content_hash = content_hash;
            }
        } else {
            // Create new Variable node
            auto node = FileNode {
                .type = NodeType::Variable,
                .name = name_id,
                .parent_dir = state.env_var_dir_id,
                .content_hash = content_hash,
            };
            auto result = ctx.graph->add_file_node(std::move(node));
            if (result) {
                state.imported_env_var_nodes[imp.var_name] = *result;
            }
        }
    }

    if (ctx.vars) {
        ctx.vars->set(imp.var_name, value);
    }

    // Track this as an imported variable for fine-grained dependency tracking
    state.imported_var_names.insert(imp.var_name);

    return {};
}

auto process_export(
    BuilderContext& ctx,
    parser::Export const& exp
) -> Result<void>
{
    // Per tup manual: "adds the environment variable VARIABLE to the export
    // list for future :-rules"
    ctx.exported_vars.insert(exp.var_name);
    return {};
}

auto expand_rule(
    BuilderContext& ctx,
    BuilderState& state,
    parser::Rule const& rule,
    std::vector<std::string> const& inputs
) -> Result<void>
{
    // Clear used vars for this rule (fine-grained dependency tracking)
    ctx.used_config_vars.clear();
    ctx.used_env_vars.clear();

    // Separate glob patterns from file inputs
    // For foreach rules, inputs may contain [pattern, file] where pattern has glob chars
    auto glob_pattern = std::string {};
    auto file_inputs = std::vector<std::string> {};
    for (auto const& inp : inputs) {
        if (parser::has_glob_chars(inp)) {
            glob_pattern = inp;
        } else {
            file_inputs.push_back(inp);
        }
    }

    // Transform inputs to Tupfile-relative paths (where commands execute from)
    auto tc = make_transform_context(ctx);
    auto cmd_inputs = std::vector<std::string> {};
    cmd_inputs.reserve(file_inputs.size());
    for (auto const& inp : file_inputs) {
        cmd_inputs.push_back(transform_input_path(*ctx.graph, tc, inp));
    }

    // Build PatternFlags once (input fields only, output fields added later)
    auto primary_input = cmd_inputs.empty() ? std::string {} : cmd_inputs[0];
    auto current_dir_name = ctx.current_dir.empty()
        ? std::string { "." }
        : ctx.current_dir.filename().string();
    auto glob_match = glob_pattern.empty() ? std::string {}
                                           : parser::glob_match_extract(glob_pattern, primary_input);

    auto flags = parser::PatternFlags {
        .input = primary_input,
        .input_base = std::string { parser::path_basename(primary_input) },
        .input_noext = std::string { parser::path_stem(primary_input) },
        .input_ext = std::string { parser::path_extension(primary_input) },
        .input_dir = current_dir_name,
        .glob_match = glob_match,
        .all_inputs = cmd_inputs,
    };

    // Early macro lookup - needed to process macro's order_only_inputs for demand-driven parsing
    auto macro_result = lookup_bang_macro(ctx, rule.command, flags);
    if (!macro_result) {
        return pup::unexpected<Error>(macro_result.error());
    }
    auto macro_ptr = *macro_result;

    // Pre-resolve order-only group references so %<group> can expand them in commands
    // This handles cross-directory groups like: | ../include/<gen-headers> |> cat %<gen-headers>
    auto rule_order_only_groups = std::unordered_map<std::string, std::vector<std::string>> {};

    // Track group NodeIds for deferred edge creation
    // Groups are first-class nodes; edges created after all Tupfiles are parsed
    auto deferred_group_ids = std::set<NodeId> {};

    // Also check regular inputs for order-only group references
    // In tup, <group> references are always order-only even when in the inputs section
    // Include macro's order_only_inputs to trigger demand-driven parsing
    auto all_inputs = std::vector<parser::PathPattern> {};
    all_inputs.insert(all_inputs.end(), rule.inputs.begin(), rule.inputs.end());
    all_inputs.insert(all_inputs.end(), rule.order_only_inputs.begin(), rule.order_only_inputs.end());
    if (macro_ptr) {
        all_inputs.insert(all_inputs.end(), macro_ptr->order_only_inputs.begin(), macro_ptr->order_only_inputs.end());
    }

    for (auto const& pattern : all_inputs) {
        if (pattern.is_order_only_group) {
            // Direct group reference: <group> or dir/<group> (is_order_only_group=true)
            auto group_dir = std::string {};
            if (!pattern.path.empty()) {
                auto expanded = parser::expand(*ctx.eval, pattern.path);
                if (expanded) {
                    group_dir = normalize_group_dir(*expanded, ctx.current_dir, ctx.options.source_root);
                }
            } else {
                group_dir = ctx.current_dir.empty() ? "." : ctx.current_dir.string();
            }

            // Demand-driven parsing: request the directory's Tupfile if not yet parsed
            request_demand_driven_parse(*ctx.eval, fs::path { group_dir });

            // Get or create the Group node (groups are first-class nodes)
            auto group_id_result = get_or_create_group_node(ctx, state, group_dir, pattern.group_name);
            if (!group_id_result) {
                continue;
            }
            auto group_id = *group_id_result;

            // Get files that are members of this group (via file → group edges)
            auto members = get_group_members(*ctx.graph, group_id);
            if (!members.empty()) {
                // Populate rule_order_only_groups for %<group> command expansion
                // Group members are outputs, so use transform_output_path for variant mapping
                auto& paths = rule_order_only_groups[pattern.group_name];
                for (auto id : members) {
                    auto path = ctx.graph->get_full_path(id);
                    if (!path.empty()) {
                        auto transformed = transform_output_path(tc, path);
                        paths.push_back(std::move(transformed));
                    }
                }
            }
            // ALWAYS defer edge creation - the group might grow as more Tupfiles are parsed
            deferred_group_ids.insert(group_id);
        } else if (!pattern.path.empty()) {
            // Path expression that may contain <group> suffix: ../include/<gen-headers>
            auto expanded = parser::expand(*ctx.eval, pattern.path);
            if (!expanded) {
                continue;
            }
            auto group_ref = parse_group_reference(*expanded, ctx.current_dir, ctx.options.source_root);
            if (group_ref) {
                // Demand-driven parsing: request the directory's Tupfile if not yet parsed
                request_demand_driven_parse(*ctx.eval, fs::path { group_ref->group_dir });

                // Get or create the Group node (groups are first-class nodes)
                auto group_id_result = get_or_create_group_node(ctx, state, group_ref->group_dir, group_ref->group_name);
                if (!group_id_result) {
                    continue;
                }
                auto group_id = *group_id_result;

                // Get files that are members of this group (via file → group edges)
                auto members = get_group_members(*ctx.graph, group_id);
                if (!members.empty()) {
                    // Populate rule_order_only_groups for %<group> command expansion
                    // Group members are outputs, so use transform_output_path for variant mapping
                    auto& paths = rule_order_only_groups[group_ref->group_name];
                    for (auto id : members) {
                        auto p = ctx.graph->get_full_path(id);
                        if (!p.empty()) {
                            auto transformed = transform_output_path(tc, p);
                            paths.push_back(std::move(transformed));
                        }
                    }
                } else {
                    // Preserve %<group> pattern literally - will be expanded in resolve_deferred_order_only_edges()
                    // This ensures the pattern isn't lost during command expansion
                    auto pattern = std::format("%<{}>", group_ref->group_name);
                    rule_order_only_groups[group_ref->group_name] = { std::move(pattern) };
                }
                // ALWAYS defer edge creation - the group might grow as more Tupfiles are parsed
                deferred_group_ids.insert(group_id);
            }
        }
    }

    // Temporarily enhance resolve_order_only_group to include this rule's groups
    // ScopeGuard ensures restoration even on early returns
    auto original_resolver = ctx.eval->resolve_order_only_group;
    auto resolver_guard = ScopeGuard([&] { ctx.eval->resolve_order_only_group = original_resolver; });
    ctx.eval->resolve_order_only_group = [&rule_order_only_groups, &original_resolver](std::string_view name
                                         ) -> std::vector<std::string> {
        // First check groups referenced by this rule (handles cross-directory)
        auto it = rule_order_only_groups.find(std::string { name });
        if (it != rule_order_only_groups.end()) {
            return it->second;
        }
        // Fall back to original resolver (local groups)
        if (original_resolver) {
            return original_resolver(name);
        }
        return {};
    };

    // Command expansion variables
    auto cmd_text = std::string {};
    auto display = std::string {};
    auto outputs_patterns = rule.outputs;

    // Use macro's outputs if rule doesn't specify any (macro_ptr set earlier)
    if (macro_ptr && outputs_patterns.empty()) {
        outputs_patterns = macro_ptr->outputs;
    }

    // Expand outputs
    auto outputs = Result<std::vector<std::string>> { expand_outputs(ctx, outputs_patterns, flags) };
    if (!outputs) {
        return pup::unexpected<Error>(outputs.error());
    }

    // Now expand command with actual outputs for %o substitution
    if (macro_ptr) {
        auto macro_cmd = Result<std::string> { expand_command(ctx, macro_ptr->command, flags, *outputs) };
        if (!macro_cmd) {
            return pup::unexpected<Error>(macro_cmd.error());
        }
        cmd_text = *macro_cmd;

        if (macro_ptr->display) {
            auto disp_result = Result<std::string> { expand_command(ctx, *macro_ptr->display, flags, *outputs) };
            if (disp_result) {
                display = *disp_result;
            }
        }
    } else {
        auto full_cmd = Result<std::string> { expand_command(ctx, rule.command, flags, *outputs) };
        if (!full_cmd) {
            return pup::unexpected<Error>(full_cmd.error());
        }
        cmd_text = *full_cmd;

        if (rule.display) {
            auto disp_result = Result<std::string> { expand_command(ctx, *rule.display, flags, *outputs) };
            if (disp_result) {
                display = *disp_result;
            }
        }
    }

    // Expand order-only inputs early so we can pass them to generated rules
    auto all_order_only = rule.order_only_inputs;
    if (macro_ptr && !macro_ptr->order_only_inputs.empty()) {
        all_order_only.insert(all_order_only.end(), macro_ptr->order_only_inputs.begin(), macro_ptr->order_only_inputs.end());
    }
    auto order_only_paths = std::vector<std::string> {};
    for (auto const& pattern : all_order_only) {
        auto order_inputs = Result<std::vector<std::string>> { expand_inputs(ctx, { pattern }) };
        if (order_inputs) {
            order_only_paths.insert(order_only_paths.end(), order_inputs->begin(), order_inputs->end());
        }
    }

    // Create command node
    auto cmd_id = Result<NodeId> { create_command_node(ctx, state, cmd_text, display) };
    if (!cmd_id) {
        return pup::unexpected<Error>(cmd_id.error());
    }

    // Check for scanner/pattern matches and generate additional rules (e.g., DEP commands)
    // Use file_inputs (excludes glob patterns) for scanner matching
    auto cmd_info = CommandInfo {
        .node_id = *cmd_id,
        .command = cmd_text,
        .display = display,
        .inputs = file_inputs,
        .order_only_inputs = order_only_paths,
        .outputs = *outputs,
        .working_dir = ctx.current_dir.string(),
    };

    // Use scanner_registry (new modular approach) if available, fall back to pattern_registry
    auto generated_rules = std::vector<GeneratedRule> {};
    if (ctx.options.scanner_registry && !ctx.options.scanner_registry->empty()) {
        generated_rules = ctx.options.scanner_registry->match_and_generate(cmd_info);
    } else if (ctx.options.pattern_registry && !ctx.options.pattern_registry->empty()) {
        generated_rules = ctx.options.pattern_registry->match_and_generate(cmd_info);
    }

    process_generated_rules(ctx, state, generated_rules, *cmd_id);

    // Create edges from inputs to command
    // Use file_inputs (excludes glob patterns which aren't valid paths)
    // Skip group references - they are handled by deferred edge resolution (order-only)
    for (auto const& input : file_inputs) {
        if (is_order_only_group_reference(input)) {
            continue;
        }
        auto input_id = Result<NodeId> { resolve_input_node(ctx, input) };
        if (!input_id) {
            return pup::unexpected<Error>(input_id.error());
        }
        auto edge_result = Result<void> { ctx.graph->add_edge(*input_id, *cmd_id) };
        if (!edge_result) {
            return pup::unexpected<Error>(edge_result.error());
        }
    }

    // Create edges from command to outputs
    for (auto const& output : *outputs) {
        auto output_id = Result<NodeId> { get_or_create_file_node(ctx, output, NodeType::Generated) };
        if (!output_id) {
            return pup::unexpected<Error>(output_id.error());
        }

        // Check for duplicate output - another command already produces this file
        auto output_inputs = ctx.graph->get_inputs(*output_id);
        if (!output_inputs.empty()) {
            for (auto input_id : output_inputs) {
                if (is_command_id(input_id)) {
                    auto existing_cmd_str_sv = get_command_str(ctx.graph->graph(), input_id);
                    auto existing_cmd_str = existing_cmd_str_sv.empty() ? "<unknown>" : std::string { existing_cmd_str_sv };
                    auto output_path = ctx.graph->get_full_path(*output_id);
                    auto err_msg = std::format(
                        "Unable to create output '{}' because it is already owned by command:\n  {}",
                        output_path,
                        existing_cmd_str
                    );
                    return make_error<void>(ErrorCode::DuplicateNode, std::move(err_msg));
                }
            }
        }

        auto edge_result = Result<void> { ctx.graph->add_edge(*cmd_id, *output_id) };
        if (!edge_result) {
            return pup::unexpected<Error>(edge_result.error());
        }

        // Add to output group {name} if specified
        auto output_group = rule.output_group;
        if (!output_group && macro_ptr && macro_ptr->output_group) {
            output_group = macro_ptr->output_group;
        }
        if (output_group) {
            ctx.groups[*output_group].push_back(*output_id);
        }

        // Add to order-only group <name> if specified
        // Supports path/<group> syntax where path specifies the group's directory
        auto output_oo_group = rule.output_order_only_group;
        if (!output_oo_group && macro_ptr && macro_ptr->output_order_only_group) {
            output_oo_group = macro_ptr->output_order_only_group;
        }
        if (output_oo_group) {
            auto dir = std::string {};

            // Get directory from path prefix if specified
            parser::Expression const* group_dir_expr = nullptr;
            if (rule.output_order_only_group_dir) {
                group_dir_expr = &*rule.output_order_only_group_dir;
            } else if (macro_ptr && macro_ptr->output_order_only_group_dir) {
                group_dir_expr = &*macro_ptr->output_order_only_group_dir;
            }

            if (group_dir_expr) {
                auto expanded = parser::expand(*ctx.eval, *group_dir_expr);
                if (expanded) {
                    // Use normalize_group_dir for consistent handling with input groups
                    // This strips variant prefix and resolves relative paths
                    dir = normalize_group_dir(*expanded, ctx.current_dir, ctx.options.source_root);
                }
            }

            if (dir.empty()) {
                dir = ctx.current_dir.empty() ? "." : ctx.current_dir.string();
            }

            // Create or get the Group node
            auto group_id_result = get_or_create_group_node(ctx, state, dir, *output_oo_group);
            if (group_id_result) {
                // Add edge: file → group (file is member of group)
                (void)ctx.graph->add_edge(*output_id, *group_id_result, LinkType::Group);
            }
        }
    }

    // Create order-only edges from the pre-expanded paths
    // Skip group references (deferred edge creation) and glob patterns (not valid paths)
    for (auto const& oi : order_only_paths) {
        if (is_order_only_group_reference(oi) || parser::has_glob_chars(oi)) {
            continue;
        }
        auto oi_id = Result<NodeId> { resolve_input_node(ctx, oi) };
        if (oi_id) {
            (void)ctx.graph->add_order_only_edge(*oi_id, *cmd_id);
        }
    }

    // Store deferred edges for groups
    // These will be resolved after all Tupfiles are parsed (group might grow)
    for (auto group_id : deferred_group_ids) {
        state.deferred_edges.insert({ group_id, *cmd_id });
    }

    return {};
}

auto expand_inputs(
    BuilderContext& ctx,
    std::vector<parser::PathPattern> const& patterns
) -> Result<std::vector<std::string>>
{
    auto result = std::vector<std::string> {};

    for (auto const& pattern : patterns) {
        if (pattern.is_exclusion || pattern.is_output_exclusion) {
            continue; // Handle exclusions later (! for inputs, ^ for foreach)
        }

        if (pattern.is_group) {
            // Bin reference {name} - local to Tupfile
            auto it = decltype(ctx.groups)::iterator { ctx.groups.find(pattern.group_name) };
            if (it != ctx.groups.end()) {
                for (auto id : it->second) {
                    auto path = ctx.graph->get_full_path(id);
                    if (!path.empty()) {
                        result.push_back(std::move(path));
                    }
                }
            }
            continue;
        }

        if (pattern.is_order_only_group) {
            // Order-only group reference <name> - cross-directory
            // The pattern.path contains the directory prefix (e.g., $(ROOT)/include/generated/)
            auto group_dir = std::string {};

            if (!pattern.path.empty()) {
                auto expanded = parser::expand(*ctx.eval, pattern.path);
                if (expanded) {
                    group_dir = normalize_group_dir(*expanded, ctx.current_dir, ctx.options.source_root);
                    request_demand_driven_parse(*ctx.eval, fs::path { group_dir });
                }
            } else {
                group_dir = ctx.current_dir.empty() ? "." : ctx.current_dir.string();
            }

            // Return the group reference string so GeneratedRules (DEP commands) can inherit it.
            // Edges are created by resolve_deferred_order_only_edges() after all Tupfiles are parsed.
            auto group_ref = group_dir.empty() ? "<" + pattern.group_name + ">"
                                               : group_dir + "/<" + pattern.group_name + ">";
            result.push_back(group_ref);
            continue;
        }

        // Expand path expression
        auto paths = parser::expand_path(*ctx.eval, pattern);
        if (!paths) {
            return pup::unexpected<Error>(paths.error());
        }

        for (auto& path : *paths) {
            // Check for path/<group> pattern (order-only group reference with directory prefix)
            auto group_ref = parse_group_reference(path, ctx.current_dir, ctx.options.source_root);
            if (group_ref) {
                request_demand_driven_parse(*ctx.eval, fs::path { group_ref->group_dir });
                // Return the group reference string so GeneratedRules (DEP commands) can inherit it.
                // Edges are created by resolve_deferred_order_only_edges() after all Tupfiles are parsed.
                result.push_back(path);
                continue;
            }
            // Include the path (pattern or literal)
            // For globs, this preserves the pattern for %g expansion in foreach rules
            if (!ctx.current_dir.empty()) {
                result.push_back((ctx.current_dir / path).lexically_normal().string());
            } else {
                result.push_back(path);
            }

            // Expand globs if enabled - add matched files after the pattern
            if (ctx.options.expand_globs && parser::has_glob_chars(path)) {
                expand_glob_pattern(ctx, path, result);
            } else if (!parser::has_glob_chars(path)) {
                // Non-glob path: trigger demand-driven parsing if file doesn't exist
                // (path already added above, but we may need to request cross-directory Tupfile)
                auto full_path = ctx.options.source_root / ctx.current_dir / path;
                if (!std::filesystem::exists(full_path)) {
                    auto file_dir = fs::path { path }.parent_path();
                    auto abs_file_dir = (ctx.current_dir / file_dir).lexically_normal();
                    request_demand_driven_parse(*ctx.eval, abs_file_dir);
                }
            }
        }
    }

    // Handle exclusions (! for regular inputs, ^ for foreach exclusions)
    apply_exclusions(ctx, patterns, result);

    return result;
}

auto expand_outputs(
    BuilderContext& ctx,
    std::vector<parser::PathPattern> const& patterns,
    parser::PatternFlags const& flags
) -> Result<std::vector<std::string>>
{
    auto result = std::vector<std::string> {};

    for (auto const& pattern : patterns) {
        if (pattern.is_group) {
            continue; // Groups are not valid in outputs
        }
        if (pattern.is_output_exclusion) {
            continue; // Exclusion patterns are markers, not actual outputs
        }

        auto paths = parser::expand_path(*ctx.eval, pattern);
        if (!paths) {
            return pup::unexpected<Error>(paths.error());
        }

        for (auto& path : *paths) {
            // Expand pattern flags (%B, %f, etc.)
            auto expanded = parser::expand_pattern(*ctx.eval, path, flags);
            auto output_path = expanded ? *expanded : std::move(path);

            // Combine with current directory and normalize
            // Output paths are relative to Tupfile directory
            auto full_output_path = (ctx.current_dir / output_path).lexically_normal().string();

            // All outputs go under BUILD_ROOT_ID.
            // This ensures Ghost nodes (created for inputs referencing not-yet-generated files)
            // unify with Generated nodes when the output is created.
            auto node_id = walk_to_file_node(
                *ctx.graph,
                BUILD_ROOT_ID,
                full_output_path,
                NodeType::Generated
            );

            if (!node_id) {
                return pup::unexpected<Error>(node_id.error());
            }

            // Get the full path from the node
            auto full_path = ctx.graph->get_full_path(*node_id);
            result.push_back(std::move(full_path));
        }
    }

    return result;
}

auto expand_command(
    BuilderContext& ctx,
    parser::Expression const& cmd,
    parser::PatternFlags flags,
    std::vector<std::string> const& outputs
) -> Result<std::string>
{
    // Expand the command expression (variable expansion)
    auto literal = parser::expand(*ctx.eval, cmd);
    if (!literal) {
        return pup::unexpected<Error>(literal.error());
    }

    auto expanded = parser::expand(*ctx.eval, std::string_view { *literal });
    if (!expanded) {
        return pup::unexpected<Error>(expanded.error());
    }

    // Transform outputs to Tupfile-relative paths and augment flags
    auto tc = make_transform_context(ctx);
    auto cmd_outputs = std::vector<std::string> {};
    cmd_outputs.reserve(outputs.size());
    for (auto const& out : outputs) {
        cmd_outputs.push_back(transform_output_path(tc, out));
    }

    // Augment flags with output fields
    auto primary_output = cmd_outputs.empty() ? std::string {} : cmd_outputs[0];
    flags.output = primary_output;
    flags.output_base = std::string { parser::path_basename(primary_output) };
    flags.all_outputs = cmd_outputs;

    // Expand pattern flags and return
    return parser::expand_pattern(*ctx.eval, *expanded, flags);
}

auto get_or_create_directory_node(
    BuilderContext& ctx,
    std::filesystem::path const& dir_path,
    int depth
) -> Result<NodeId>
{
    // Normalize first to handle ., .., and redundant separators
    auto normalized_path = dir_path.lexically_normal();

    // Root directory (empty, ".", or "/") has no parent - return 0
    if (normalized_path.empty() || normalized_path == "." || normalized_path == "/") {
        return NodeId { 0 };
    }

    // Guard against pathological recursion
    if (depth > MAX_DIRECTORY_DEPTH) {
        return make_error<NodeId>(ErrorCode::InvalidArgument, "Directory nesting exceeds maximum depth");
    }

    auto parent_path = normalized_path.parent_path();
    auto basename = normalized_path.filename().string();

    // Recurse to get/create parent directory
    auto parent_id_result = get_or_create_directory_node(ctx, parent_path, depth + 1);
    if (!parent_id_result) {
        return parent_id_result;
    }
    auto parent_id = *parent_id_result;

    // Check if directory already exists
    if (auto existing = ctx.graph->find_by_dir_name(parent_id, basename)) {
        return *existing;
    }

    // Create new directory node
    auto node = FileNode {
        .type = NodeType::Directory,
        .name = ctx.graph->intern(basename),
        .parent_dir = parent_id,
    };

    return ctx.graph->add_file_node(std::move(node));
}

auto get_or_create_file_node(
    BuilderContext& ctx,
    std::string const& path,
    NodeType type
) -> Result<NodeId>
{
    // Convert working-directory-relative paths to source-root-relative or absolute
    // Paths like "../../build/foo" from "src/bar" should become "build/foo"
    // For Generated nodes, first check if path already has build root prefix.
    // This happens when expand_outputs returns paths like "../build/lib/add.o".
    // We must strip the prefix and look up under BUILD_ROOT_ID before any other
    // path manipulation that could corrupt the lookup.
    auto build_root_name = ctx.graph->get_build_root_name();
    if (type == NodeType::Generated && !build_root_name.empty()) {
        auto lookup_path = strip_build_prefix(path, build_root_name);
        if (lookup_path != path) { // Had prefix
            if (auto existing = ctx.graph->find_by_path(lookup_path, BUILD_ROOT_ID)) {
                return *existing;
            }
        }
    }

    // Paths that escape source root become absolute for correct stat() resolution
    auto resolved = std::string { path };
    if (!ctx.current_dir.empty() && path.starts_with("..")) {
        auto normalized = (ctx.current_dir / path).lexically_normal();
        // Check if path escapes source root (starts with ..)
        if (normalized.string().starts_with("..")) {
            // Use absolute path for out-of-tree files (without resolving symlinks)
            auto abs = (ctx.options.source_root / normalized).lexically_normal();
            resolved = abs.string();
        } else {
            resolved = normalized.string();
        }
    }

    // Normalize path for consistent lookup (handles //, ., ..)
    auto normalized = normalize_path(resolved);

    // For Generated nodes, check if node was already created under BUILD_ROOT_ID
    // by expand_outputs. This handles paths without the build prefix.
    if (type == NodeType::Generated && !build_root_name.empty()) {
        auto lookup_path = strip_build_prefix(normalized, build_root_name);
        if (auto existing = ctx.graph->find_by_path(lookup_path, BUILD_ROOT_ID)) {
            return *existing;
        }
    }

    // For Generated nodes, use walk_to_file_node to ensure they're created under BUILD_ROOT_ID.
    // This maintains consistency with expand_outputs() which also uses BUILD_ROOT_ID.
    if (type == NodeType::Generated) {
        return walk_to_file_node(*ctx.graph, BUILD_ROOT_ID, normalized, NodeType::Generated);
    }

    auto fs_path = fs::path { normalized };
    auto basename = fs_path.filename().string();

    // Get or create parent directory node
    auto parent_path = fs_path.parent_path();
    auto parent_id_result = get_or_create_directory_node(ctx, parent_path);
    if (!parent_id_result) {
        return parent_id_result;
    }
    auto parent_id = *parent_id_result;

    // Check if node already exists
    if (auto existing = ctx.graph->find_by_dir_name(parent_id, basename)) {
        return *existing;
    }

    // Create new node
    auto node = FileNode {
        .type = type,
        .name = ctx.graph->intern(basename),
        .parent_dir = parent_id,
    };

    return ctx.graph->add_file_node(std::move(node));
}

auto resolve_input_node(
    BuilderContext& ctx,
    std::string const& path
) -> Result<NodeId>
{
    // Input paths are already source-relative from expand_inputs() which normalizes them
    // by combining with current_dir. No further normalization needed here.

    // For variant builds, paths like "build/include/header.h" (from $(B)/include/header.h)
    // already have the build root prefix. Strip it to get source-relative paths.
    auto build_root_name = ctx.graph->get_build_root_name();
    auto normalized_path = strip_build_prefix(path, build_root_name);

    // With BUILD_ROOT_ID model:
    // - Source files are under SOURCE_ROOT_ID (0) at source-relative paths
    // - Generated/Ghost files are under BUILD_ROOT_ID at source-relative paths

    // First check if node exists under BUILD_ROOT_ID (generated files)
    if (auto existing = ctx.graph->find_by_path(normalized_path, BUILD_ROOT_ID)) {
        return *existing;
    }

    // Check under SOURCE_ROOT_ID (source files)
    if (auto existing = ctx.graph->find_by_path(normalized_path, SOURCE_ROOT_ID)) {
        return *existing;
    }

    // Node doesn't exist - check filesystem to determine type
    auto source_path = ctx.options.source_root / normalized_path;
    if (fs::exists(source_path)) {
        // Source file exists - create File node under SOURCE_ROOT_ID
        return walk_to_file_node(*ctx.graph, SOURCE_ROOT_ID, normalized_path, NodeType::File);
    }

    // Check if file exists in build directory (e.g., tup.config, or already-generated files)
    auto build_path = ctx.options.output_root / normalized_path;
    if (fs::exists(build_path)) {
        // File exists in build dir but not source - it's a Generated output from a previous build.
        // Create as Ghost so the rule that generates it can upgrade it to Generated.
        // (If the rule no longer generates it, the Ghost remains and causes an error.)
        return walk_to_file_node(*ctx.graph, BUILD_ROOT_ID, normalized_path, NodeType::Ghost);
    }

    // File doesn't exist anywhere - create Ghost node under BUILD_ROOT_ID
    // Ghost nodes represent not-yet-generated files, which will be under build root
    return walk_to_file_node(*ctx.graph, BUILD_ROOT_ID, normalized_path, NodeType::Ghost);
}

auto get_or_create_group_node(
    BuilderContext& ctx,
    BuilderState& state,
    std::string const& directory,
    std::string const& name
) -> Result<NodeId>
{
    // Check cache first (fast path)
    auto key = GroupKey { directory, name };
    auto it = state.group_nodes.find(key);
    if (it != state.group_nodes.end()) {
        return it->second;
    }

    // Get or create parent directory node
    auto parent_id_result = get_or_create_directory_node(ctx, directory);
    if (!parent_id_result) {
        return parent_id_result;
    }
    auto parent_id = *parent_id_result;

    // Check if group node already exists in graph (e.g., from previous Tupfile)
    // Group nodes are stored with angle-bracket name like "<gen-headers>"
    auto group_basename = "<" + name + ">";
    if (auto existing = ctx.graph->find_by_dir_name(parent_id, group_basename)) {
        state.group_nodes[key] = *existing;
        return *existing;
    }

    // Create new group node
    auto node = FileNode {
        .type = NodeType::Group,
        .name = ctx.graph->intern(group_basename),
        .parent_dir = parent_id,
    };

    auto result = ctx.graph->add_file_node(std::move(node));
    if (result) {
        state.group_nodes[key] = *result;
    }
    return result;
}

auto create_command_node(
    BuilderContext& ctx,
    BuilderState& state,
    std::string const& command,
    std::string const& display
) -> Result<NodeId>
{
    // Intern exported_vars
    auto exported_var_ids = std::set<StringId> {};
    for (auto const& var : ctx.exported_vars) {
        exported_var_ids.insert(ctx.graph->intern(var));
    }

    auto node = CommandNode {
        .command = ctx.graph->intern(command),
        .display = ctx.graph->intern(display),
        .source_dir = ctx.graph->intern(ctx.current_dir.string()),
        .exported_vars = std::move(exported_var_ids),
    };

    auto cmd_id_result = ctx.graph->add_command_node(std::move(node));
    if (!cmd_id_result) {
        return cmd_id_result;
    }

    auto cmd_id = *cmd_id_result;

    // Add sticky edges from Tupfile and included files to this command
    for (auto src_id : ctx.sticky_sources) {
        (void)ctx.graph->add_edge(src_id, cmd_id, LinkType::Sticky);
    }

    // Add sticky edges from used config variables (fine-grained dependency tracking)
    for (auto const& var_name : ctx.used_config_vars) {
        auto it = state.config_var_nodes.find(var_name);
        if (it != state.config_var_nodes.end()) {
            (void)ctx.graph->add_edge(it->second, cmd_id, LinkType::Sticky);
        }
    }

    // Add sticky edges from used imported env variables (fine-grained dependency tracking)
    for (auto const& var_name : ctx.used_env_vars) {
        auto it = state.imported_env_var_nodes.find(var_name);
        if (it != state.imported_env_var_nodes.end()) {
            (void)ctx.graph->add_edge(it->second, cmd_id, LinkType::Sticky);
        }
    }

    return cmd_id;
}

} // anonymous namespace

// ============================================================================
// Public free function API
// ============================================================================

auto make_builder_state(BuilderOptions opts) -> BuilderState
{
    return BuilderState {
        .options = std::move(opts),
        .errors = {},
        .warnings = {},
        .group_nodes = {},
        .deferred_edges = {},
        .config_var_nodes = {},
        .env_var_dir_id = INVALID_NODE_ID,
        .imported_env_var_nodes = {},
        .imported_var_names = {},
        .var_config_deps = {},
        .var_env_deps = {},
    };
}

auto build_graph(
    parser::Tupfile const& tupfile,
    parser::EvalContext& eval,
    BuilderState& state
) -> Result<BuildGraph>
{
    auto graph = BuildGraph {};

    // Set build root name: relative path from source to output root.
    // For in-tree builds (source == output), this is empty.
    // For variant builds (-B build), this is "build".
    if (state.options.source_root != state.options.output_root) {
        auto build_root_name = fs::relative(state.options.output_root, state.options.source_root).string();
        graph.set_build_root_name(std::move(build_root_name));
    }

    auto result = Result<void> { add_tupfile(graph, tupfile, eval, state) };
    if (!result) {
        return pup::unexpected<Error>(result.error());
    }
    return graph;
}

auto add_tupfile(
    BuildGraph& graph,
    parser::Tupfile const& tupfile,
    parser::EvalContext& eval,
    BuilderState& state
) -> Result<void>
{
    // Compute current_dir relative to config_root (where Tupfiles live)
    // In 3-tree builds, config_root differs from source_root, but the directory
    // structure mirrors the source tree, so this relative path is used for both
    // config lookup and source file glob expansion.
    auto const& tupfile_root = state.options.config_root.empty()
        ? state.options.source_root
        : state.options.config_root;
    auto tupfile_parent = std::filesystem::path { tupfile.filename }.parent_path();
    auto relative_dir = std::filesystem::relative(tupfile_parent, tupfile_root);
    if (relative_dir == ".") {
        relative_dir = "";
    }

    auto ctx = BuilderContext {
        .graph = &graph,
        .eval = &eval,
        .vars = eval.vars,
        .options = state.options,
        .current_dir = relative_dir,
        .current_file = tupfile.filename,
    };

    // Create Tupfile node and add to sticky_sources for dependency tracking
    // For 3-tree builds, store relative to config_root (Tupfile's actual location)
    auto tupfile_rel = std::filesystem::relative(tupfile.filename, tupfile_root).string();
    auto tupfile_node_result = get_or_create_file_node(ctx, tupfile_rel, NodeType::File);
    if (tupfile_node_result) {
        ctx.sticky_sources.push_back(*tupfile_node_result);
    }

    // Create Variable nodes for fine-grained config dependency tracking
    // Each config variable becomes a node so commands only depend on variables they use
    // Only create nodes once (first Tupfile); subsequent Tupfiles reuse existing nodes
    if (eval.config_vars && state.config_var_nodes.empty()) {
        // Get config directory for Variable node parent (typically the -B directory)
        auto config_dir_id = NodeId { 0 };
        if (!state.options.config_path.empty()) {
            auto config_parent = std::filesystem::path { state.options.config_path }.parent_path();
            auto config_dir_rel = std::filesystem::relative(config_parent, state.options.source_root).string();
            if (config_dir_rel.empty() || config_dir_rel == ".") {
                config_dir_rel = "";
            }
            auto dir_result = get_or_create_directory_node(ctx, config_dir_rel);
            if (dir_result) {
                config_dir_id = *dir_result;
            }
        }

        for (auto const& var_name : eval.config_vars->names()) {
            // Skip CONFIG_ prefixed names (we store the stripped version)
            if (var_name.starts_with(parser::builtin_vars::CONFIG_)) {
                continue;
            }

            // Check if node already exists (shouldn't happen with empty check above, but defensive)
            if (auto existing = graph.find_by_dir_name(config_dir_id, var_name)) {
                state.config_var_nodes[std::string { var_name }] = *existing;
                continue;
            }

            auto value = eval.config_vars->get(var_name);
            auto node = FileNode {
                .type = NodeType::Variable,
                .name = graph.intern(var_name),
                .parent_dir = config_dir_id,
                .content_hash = sha256(value),
            };

            auto var_id_result = graph.add_file_node(std::move(node));
            if (var_id_result) {
                state.config_var_nodes[std::string { var_name }] = *var_id_result;
            }
        }
    }

    // Create/find virtual $ directory for imported env vars (like tup's env_dt)
    // Only initialize once; subsequent Tupfiles reuse existing nodes
    if (state.env_var_dir_id == INVALID_NODE_ID) {
        // Check if $ directory already exists in graph (from same build session)
        if (auto existing = graph.find_by_dir_name(NodeId { 0 }, "$")) {
            state.env_var_dir_id = *existing;
        } else {
            // Create new $ directory under root
            auto env_dir_node = FileNode {
                .type = NodeType::Directory,
                .name = graph.intern("$"),
                .parent_dir = NodeId { 0 },
            };
            auto result = graph.add_file_node(std::move(env_dir_node));
            if (result) {
                state.env_var_dir_id = *result;
            }
        }
    }

    // Set up callback to track which config variables are used during expansion
    eval.on_config_var_used = [&ctx](std::string_view name) {
        ctx.used_config_vars.insert(std::string { name });
    };

    // Set up callback to track which imported env variables are used during expansion
    eval.imported_vars = &state.imported_var_names;
    eval.on_env_var_used = [&ctx](std::string_view name) {
        ctx.used_env_vars.insert(std::string { name });
    };

    // Wire up transitive dependency maps for variable tracking
    // When $(CXXFLAGS) is expanded and CXXFLAGS depends on @(RELEASE_CXXFLAGS),
    // the propagation in eval.cpp will call on_config_var_used("RELEASE_CXXFLAGS")
    eval.var_config_deps = &state.var_config_deps;
    eval.var_env_deps = &state.var_env_deps;

    // Set up resolve_group callback for {group} pattern expansion
    eval.resolve_group = [&ctx](std::string_view name
                         ) -> std::vector<std::string> {
        auto it = ctx.groups.find(std::string { name });
        if (it == ctx.groups.end()) {
            return {};
        }
        auto paths = std::vector<std::string> {};
        for (auto id : it->second) {
            auto path = ctx.graph->get_full_path(id);
            if (!path.empty()) {
                paths.push_back(std::move(path));
            }
        }
        return paths;
    };

    // Set up resolve_order_only_group callback for %<group> pattern expansion in commands
    // This is for local group references (no directory prefix) - uses current directory
    // Groups are first-class nodes; lookup via graph edges (file → group)
    eval.resolve_order_only_group = [&ctx, &state](std::string_view name
                                    ) -> std::vector<std::string> {
        auto dir = ctx.current_dir.empty() ? "." : ctx.current_dir.string();
        auto key = GroupKey { dir, std::string { name } };
        auto it = state.group_nodes.find(key);
        if (it == state.group_nodes.end()) {
            return {};
        }
        auto paths = std::vector<std::string> {};
        auto members = get_group_members(*ctx.graph, it->second);
        for (auto id : members) {
            auto path = ctx.graph->get_full_path(id);
            if (!path.empty()) {
                paths.push_back(std::move(path));
            }
        }
        return paths;
    };

    for (auto const& stmt : tupfile.statements) {
        auto result = Result<void> { process_statement(ctx, state, *stmt) };
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

auto resolve_deferred_order_only_edges(
    BuildGraph& graph,
    BuilderState& state
) -> Result<void>
{
    // Resolve deferred order-only edges
    // With groups as first-class nodes, we create a single edge: group → command
    for (auto const& edge : state.deferred_edges) {
        // Verify group node exists and has members
        auto const* group_node = graph.get_file_node(edge.group_id);
        if (!group_node || group_node->type != NodeType::Group) {
            continue;
        }

        auto members = get_group_members(graph, edge.group_id);
        if (!members.empty()) {
            // Create single order-only edge: group → command
            (void)graph.add_order_only_edge(edge.group_id, edge.command_id);

            // Expand %<group> pattern in command string (was preserved during parsing)
            // Extract group name from node's basename (e.g., "<archives>" -> "archives")
            auto group_basename = std::string { graph.str(group_node->name) };
            if (group_basename.size() > 2 && group_basename.front() == '<' && group_basename.back() == '>') {
                auto group_name = group_basename.substr(1, group_basename.size() - 2);
                auto pattern = std::format("%<{}>", group_name);

                // Get command node and check if pattern exists
                auto* cmd_node = graph.get_command_node(edge.command_id);
                auto cmd_str = std::string { graph.str(cmd_node->command) };
                if (cmd_node && cmd_str.find(pattern) != std::string::npos) {
                    // Construct path transform context from command's source_dir
                    auto source_dir_str = std::string { graph.str(cmd_node->source_dir) };
                    auto current_dir = fs::path { source_dir_str };
                    auto tc = PathTransformContext {
                        .source_to_root = compute_source_to_root(current_dir),
                        .current_dir_str = source_dir_str,
                        .source_root = state.options.source_root,
                        .output_root = state.options.output_root,
                    };

                    // Transform member paths and build replacement string
                    auto replacement = std::string {};
                    for (auto id : members) {
                        auto p = graph.get_full_path(id);
                        if (!p.empty()) {
                            if (!replacement.empty()) {
                                replacement += ' ';
                            }
                            replacement += transform_output_path(tc, p);
                        }
                    }

                    // Replace pattern in command
                    auto pos = cmd_str.find(pattern);
                    while (pos != std::string::npos) {
                        cmd_str.replace(pos, pattern.size(), replacement);
                        pos = cmd_str.find(pattern, pos + replacement.size());
                    }
                    cmd_node->command = graph.intern(cmd_str);

                    // Also update display if it contains the pattern
                    auto display_str = std::string { graph.str(cmd_node->display) };
                    if (display_str.find(pattern) != std::string::npos) {
                        pos = display_str.find(pattern);
                        while (pos != std::string::npos) {
                            display_str.replace(pos, pattern.size(), replacement);
                            pos = display_str.find(pattern, pos + replacement.size());
                        }
                        cmd_node->display = graph.intern(display_str);
                    }
                }
            }
        } else {
            // Group exists but has no members - warn about potential typo
            auto group_path = graph.get_full_path(edge.group_id);
            state.warnings.push_back(std::format("order-only group {} has no members", group_path));
        }
    }

    // Clear deferred edges after resolution
    state.deferred_edges.clear();

    return {};
}

// ============================================================================
// GraphBuilder wrapper implementation
// ============================================================================

GraphBuilder::GraphBuilder(BuilderOptions options)
    : state_ { make_builder_state(std::move(options)) }
{
}

auto GraphBuilder::errors() const -> std::vector<std::string> const&
{
    return state_.errors;
}

auto GraphBuilder::warnings() const -> std::vector<std::string> const&
{
    return state_.warnings;
}

auto GraphBuilder::build(
    parser::Tupfile const& tupfile,
    parser::EvalContext& eval
) -> Result<BuildGraph>
{
    return build_graph(tupfile, eval, state_);
}

auto GraphBuilder::add_tupfile(
    BuildGraph& graph,
    parser::Tupfile const& tupfile,
    parser::EvalContext& eval
) -> Result<void>
{
    return pup::graph::add_tupfile(graph, tupfile, eval, state_);
}

auto GraphBuilder::resolve_deferred_order_only_edges(BuildGraph& graph) -> Result<void>
{
    return pup::graph::resolve_deferred_order_only_edges(graph, state_);
}

auto GraphBuilder::state() -> BuilderState&
{
    return state_;
}

auto GraphBuilder::state() const -> BuilderState const&
{
    return state_;
}

} // namespace pup::graph
