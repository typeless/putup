// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/graph/builder.hpp"
#include "pup/core/hash.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/rule_pattern.hpp"
#include "pup/parser/eval.hpp"
#include "pup/parser/glob.hpp"
#include "pup/parser/parser.hpp"

#include <fmt/core.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace pup::graph {

namespace fs = std::filesystem;

// Debug output for order-only group registration
constexpr bool DEBUG_OO_GROUPS = false;

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

/// Normalize a directory path for group key lookup
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

    auto result = path.empty() ? "." : path.string();
    if constexpr (DEBUG_OO_GROUPS) {
        fmt::print(stderr, "[DEBUG] normalize_group_dir({}, {}) -> {}\n", path_str, current_dir.string(), result);
    }
    return result;
}

/// Map an output path to the output directory.
/// All paths are project-relative (tup-style), never absolute.
/// For in-tree builds: current_dir/path (e.g., "src/lib/foo.o")
/// For out-of-tree builds: output_prefix/current_dir/path (e.g., "build/src/lib/foo.o")
auto map_to_output(
    std::string const& path,
    std::filesystem::path const& current_dir,
    std::filesystem::path const& source_root,
    std::filesystem::path const& output_root
) -> std::string
{
    auto p = std::filesystem::path { path };

    // If path is already absolute, make it relative to source_root
    if (p.is_absolute()) {
        auto rel = fs::relative(p, source_root);
        if (!rel.empty() && rel.string()[0] != '.') {
            return rel.lexically_normal().string();
        }
        return p.lexically_normal().string();
    }

    // Out-of-tree build: outputs go to output_root
    // Compute output prefix as relative path from source_root to output_root
    if (!output_root.empty() && source_root != output_root) {
        auto output_prefix = fs::relative(output_root, source_root);
        auto output_prefix_str = output_prefix.string();
        if (current_dir.empty()) {
            return (output_prefix / path).lexically_normal().string();
        }

        // If path escapes current_dir (starts with ../), resolve it first
        // to check if it already goes to the output directory
        // Note: Must check for ".." or "../" specifically, not just ".." prefix
        // because "..hidden" is a valid filename, not a parent reference
        auto is_parent_ref = (path == ".." || (path.size() > 2 && path[0] == '.' && path[1] == '.' && path[2] == '/'));
        if (is_parent_ref) {
            auto resolved = (current_dir / path).lexically_normal().string();
            // If resolved path already starts with output_prefix, don't add it again
            if (resolved.size() >= output_prefix_str.size()
                && std::string_view { resolved }.substr(0, output_prefix_str.size()) == output_prefix_str
                && (resolved.size() == output_prefix_str.size() || resolved[output_prefix_str.size()] == '/')) {
                return resolved;
            }
        }

        return (output_prefix / current_dir / path).lexically_normal().string();
    }

    // In-tree build: paths are project-relative
    if (current_dir.empty()) {
        return path;
    }
    return (current_dir / path).lexically_normal().string();
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
    if (path.size() >= 2 && path[0] == '.' && path[1] == '.') {
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

/// Context for transforming paths to Tupfile-relative coordinates.
///
/// Commands execute from the Tupfile's source directory, so all paths in commands
/// must be relative to that directory. This requires:
/// 1. Mapping variant outputs to the output directory (e.g., build/src/foo.o)
/// 2. Converting project-root-relative paths to Tupfile-relative (e.g., ../lib/bar.c)
///
/// This context is computed once per rule and reused for both inputs and outputs,
/// avoiding redundant computation of source_to_root. The distinction between variant
/// and non-variant builds is handled by map_to_output() checking source_root == output_root.
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

/// Transform an input path to Tupfile-relative, applying variant mapping if needed.
/// Generated nodes are mapped to the output directory; source files are not.
/// For unknown paths, check filesystem: if file exists only in variant, map it.
auto transform_input_path(
    BuilderContext& ctx,
    PathTransformContext const& tc,
    std::string const& inp
) -> std::string
{
    auto path = inp;
    auto needs_mapping = false;

    if (auto node_id = ctx.graph->find_by_path(inp)) {
        // Known node - map if generated
        auto* node = ctx.graph->get_node(*node_id);
        if (node && node->type == NodeType::Generated) {
            needs_mapping = true;
        }
    } else {
        // Unknown path - check filesystem for variant-only files
        auto source_path = tc.source_root / inp;
        if (!fs::exists(source_path)) {
            auto variant_path = tc.output_root / inp;
            if (fs::exists(variant_path)) {
                needs_mapping = true;
            }
        }
    }

    if (needs_mapping) {
        path = map_to_output(inp, fs::path {}, tc.source_root, tc.output_root);
    }

    return make_source_relative(path, tc.source_to_root, tc.current_dir_str);
}

/// Transform an output path to Tupfile-relative, applying variant mapping.
/// map_to_output handles both variant (S!=B) and non-variant (S==B) cases.
auto transform_output_path(
    PathTransformContext const& tc,
    std::string const& out
) -> std::string
{
    auto path = map_to_output(out, fs::path {}, tc.source_root, tc.output_root);
    return make_source_relative(path, tc.source_to_root, tc.current_dir_str);
}

/// Get all files that are members of a group (via file → group edges)
/// Returns file NodeIds by finding all input edges to the group node
auto get_group_members(BuildGraph& graph, NodeId group_id) -> std::vector<NodeId>
{
    // Files point TO groups via Group edges (file → group)
    // So group.inputs contains the member files
    return graph.get_inputs(group_id);
}

/// RAII scope guard for cleanup on scope exit
struct ScopeGuard {
    std::function<void()> cleanup;
    explicit ScopeGuard(std::function<void()> fn)
        : cleanup { std::move(fn) }
    {
    }
    ~ScopeGuard()
    {
        if (cleanup) {
            cleanup();
        }
    }
    ScopeGuard(ScopeGuard const&) = delete;
    auto operator=(ScopeGuard const&) -> ScopeGuard& = delete;
    ScopeGuard(ScopeGuard&&) = delete;
    auto operator=(ScopeGuard&&) -> ScopeGuard& = delete;
};

} // namespace

struct GraphBuilder::Impl {
    struct GroupKey {
        std::string directory;
        std::string name;

        auto operator==(GroupKey const& other) const -> bool = default;
        auto operator<(GroupKey const& other) const -> bool
        {
            return std::tie(directory, name) < std::tie(other.directory, other.name);
        }
    };

    struct GroupKeyHash {
        auto operator()(GroupKey const& k) const -> std::size_t
        {
            auto h1 = std::hash<std::string> {}(k.directory);
            auto h2 = std::hash<std::string> {}(k.name);
            return h1 ^ (h2 << 1);
        }
    };

    /// Deferred order-only edge reference for circular parsing situations
    /// Stores group NodeId (not GroupKey) for direct edge creation
    struct DeferredOrderOnlyEdge {
        NodeId group_id;
        NodeId command_id;

        auto operator<(DeferredOrderOnlyEdge const& other) const -> bool
        {
            return std::tie(group_id, command_id) < std::tie(other.group_id, other.command_id);
        }
    };

    BuilderOptions options;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    /// Group node lookup: (directory, name) → NodeId
    /// Used for quick lookup when groups are referenced before creation
    std::unordered_map<GroupKey, NodeId, GroupKeyHash> group_nodes;

    /// Deferred edges to resolve after all Tupfiles are parsed
    /// Using set to avoid duplicate edges when same group referenced multiple times
    std::set<DeferredOrderOnlyEdge> deferred_edges;

    /// Config variable nodes (name -> NodeId) for fine-grained dependency tracking
    /// Persists across add_tupfile calls to avoid duplicate nodes
    std::unordered_map<std::string, NodeId> config_var_nodes;

    /// Virtual $ directory for imported environment variables (like tup's env_dt)
    NodeId env_var_dir_id = INVALID_NODE_ID;

    /// Imported environment variable nodes (var_name -> NodeId)
    /// Node name encodes "key=value" for persistence
    std::unordered_map<std::string, NodeId> imported_env_var_nodes;

    /// Set of imported variable names (for tracking which vars are imported)
    std::unordered_set<std::string> imported_var_names;
};

GraphBuilder::GraphBuilder(BuilderOptions options)
    : impl_(std::make_unique<Impl>())
{
    impl_->options = std::move(options);
}

GraphBuilder::~GraphBuilder() = default;

GraphBuilder::GraphBuilder(GraphBuilder&&) noexcept = default;

auto GraphBuilder::operator=(GraphBuilder&&) noexcept -> GraphBuilder& = default;

auto GraphBuilder::errors() const -> std::vector<std::string> const&
{
    return impl_->errors;
}

auto GraphBuilder::warnings() const -> std::vector<std::string> const&
{
    return impl_->warnings;
}

auto GraphBuilder::build(
    parser::Tupfile const& tupfile,
    parser::EvalContext& eval
) -> Result<BuildGraph>
{
    auto graph = BuildGraph {};
    auto result = Result<void> { add_tupfile(graph, tupfile, eval) };
    if (!result) {
        return pup::unexpected<Error>(result.error());
    }
    return graph;
}

auto GraphBuilder::add_tupfile(
    BuildGraph& graph,
    parser::Tupfile const& tupfile,
    parser::EvalContext& eval
) -> Result<void>
{
    // Compute current_dir relative to source_root
    auto tupfile_parent = std::filesystem::path { tupfile.filename }.parent_path();
    auto relative_dir = std::filesystem::relative(tupfile_parent, impl_->options.source_root);
    if (relative_dir == ".") {
        relative_dir = "";
    }

    auto ctx = BuilderContext {
        .graph = &graph,
        .eval = &eval,
        .vars = eval.vars,
        .options = impl_->options,
        .current_dir = relative_dir,
        .current_file = tupfile.filename,
    };

    // Create Tupfile node and add to sticky_sources for dependency tracking
    auto tupfile_rel = std::filesystem::relative(tupfile.filename, impl_->options.source_root).string();
    auto tupfile_node_result = get_or_create_file_node(ctx, tupfile_rel, NodeType::File);
    if (tupfile_node_result) {
        ctx.sticky_sources.push_back(*tupfile_node_result);
    }

    // Create Variable nodes for fine-grained config dependency tracking
    // Each config variable becomes a node so commands only depend on variables they use
    // Only create nodes once (first Tupfile); subsequent Tupfiles reuse existing nodes
    if (eval.config_vars && impl_->config_var_nodes.empty()) {
        // Get config directory for Variable node parent (typically the -B directory)
        auto config_dir_id = NodeId { 0 };
        if (!impl_->options.config_path.empty()) {
            auto config_parent = std::filesystem::path { impl_->options.config_path }.parent_path();
            auto config_dir_rel = std::filesystem::relative(config_parent, impl_->options.source_root).string();
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
                impl_->config_var_nodes[std::string { var_name }] = *existing;
                continue;
            }

            auto value = eval.config_vars->get(var_name);
            auto node = Node {
                .type = NodeType::Variable,
                .name = std::string { var_name },
                .parent_dir = config_dir_id,
                .content_hash = sha256(value),
            };

            auto var_id_result = graph.add_node(std::move(node));
            if (var_id_result) {
                impl_->config_var_nodes[std::string { var_name }] = *var_id_result;
            }
        }
    }

    // Create/find virtual $ directory for imported env vars (like tup's env_dt)
    // Only initialize once; subsequent Tupfiles reuse existing nodes
    if (impl_->env_var_dir_id == INVALID_NODE_ID) {
        // Check if $ directory already exists in graph (from same build session)
        if (auto existing = graph.find_by_dir_name(NodeId { 0 }, "$")) {
            impl_->env_var_dir_id = *existing;
        } else {
            // Create new $ directory under root
            auto env_dir_node = Node {
                .type = NodeType::Directory,
                .name = "$",
                .parent_dir = NodeId { 0 },
            };
            auto result = graph.add_node(std::move(env_dir_node));
            if (result) {
                impl_->env_var_dir_id = *result;
            }
        }
    }

    // Set up callback to track which config variables are used during expansion
    eval.on_config_var_used = [&ctx](std::string_view name) {
        ctx.used_config_vars.insert(std::string { name });
    };

    // Set up callback to track which imported env variables are used during expansion
    eval.imported_vars = &impl_->imported_var_names;
    eval.on_env_var_used = [&ctx](std::string_view name) {
        ctx.used_env_vars.insert(std::string { name });
    };

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
    eval.resolve_order_only_group = [this, &ctx](std::string_view name
                                    ) -> std::vector<std::string> {
        auto dir = ctx.current_dir.empty() ? "." : ctx.current_dir.string();
        auto key = Impl::GroupKey { dir, std::string { name } };
        auto it = impl_->group_nodes.find(key);
        if (it == impl_->group_nodes.end()) {
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
        auto result = Result<void> { process_statement(ctx, *stmt) };
        if (!result) {
            impl_->errors.push_back(result.error().message);
            if (!impl_->options.verbose) {
                return pup::unexpected<Error>(result.error());
            }
        }
    }

    // Apply pending weak assignments (??=) - last wins
    // Process in reverse order so earlier assignments are checked against later ones
    if (ctx.vars && !ctx.pending_weak_assignments.empty()) {
        for (auto it = ctx.pending_weak_assignments.rbegin();
             it != ctx.pending_weak_assignments.rend();
             ++it) {
            if (!ctx.vars->contains(it->first)) {
                ctx.vars->set(it->first, it->second);
            }
        }
    }

    // Copy errors and warnings
    for (auto& err : ctx.errors) {
        impl_->errors.push_back(std::move(err));
    }
    for (auto& warn : ctx.warnings) {
        impl_->warnings.push_back(std::move(warn));
    }

    return {};
}

auto GraphBuilder::process_statement(
    BuilderContext& ctx,
    parser::Statement const& stmt
) -> Result<void>
{
    if (auto const* rule = stmt.as<parser::Rule>()) {
        return process_rule(ctx, *rule);
    }

    if (auto const* macro = stmt.as<parser::BangMacro>()) {
        return process_bang_macro(ctx, *macro);
    }

    if (auto const* assign = stmt.as<parser::Assignment>()) {
        return process_assignment(ctx, *assign);
    }

    if (auto const* cond = stmt.as<parser::Conditional>()) {
        return process_conditional(ctx, *cond);
    }

    if (auto const* inc = stmt.as<parser::Include>()) {
        return process_include(ctx, *inc);
    }

    if (auto const* imp = stmt.as<parser::Import>()) {
        return process_import(ctx, *imp);
    }

    if (auto const* exp = stmt.as<parser::Export>()) {
        return process_export(ctx, *exp);
    }

    // Other directives (preload, run, error) not yet implemented
    return {};
}

auto GraphBuilder::process_rule(
    BuilderContext& ctx,
    parser::Rule const& rule
) -> Result<void>
{
    // Apply any pending weak assignments (??=) before expanding commands
    // This ensures ??= assignments that precede rules take effect
    if (ctx.vars && !ctx.pending_weak_assignments.empty()) {
        for (auto it = ctx.pending_weak_assignments.rbegin();
             it != ctx.pending_weak_assignments.rend();
             ++it) {
            if (!ctx.vars->contains(it->first)) {
                ctx.vars->set(it->first, it->second);
            }
        }
        ctx.pending_weak_assignments.clear();
    }

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
            auto result = Result<void> { expand_rule(ctx, rule, iter_inputs) };
            if (!result) {
                return pup::unexpected<Error>(result.error());
            }
        }
    } else {
        // Normal rule: single command for all inputs
        auto result = Result<void> { expand_rule(ctx, rule, *inputs) };
        if (!result) {
            return pup::unexpected<Error>(result.error());
        }
    }

    return {};
}

auto GraphBuilder::process_bang_macro(
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

auto GraphBuilder::process_assignment(
    BuilderContext& ctx,
    parser::Assignment const& assign
) -> Result<void>
{
    auto evaluator = parser::Evaluator { ctx.eval };

    // Evaluate the variable name (may contain variable refs like foo-$(BAR))
    auto name = Result<std::string> { evaluator.expand(assign.name) };
    if (!name) {
        return pup::unexpected<Error>(name.error());
    }

    // Evaluate the value
    auto value = Result<std::string> { evaluator.expand(assign.value) };
    if (!value) {
        return pup::unexpected<Error>(value.error());
    }

    // Config variables are read-only (loaded from tup.config), so only Regular and Node are writable
    auto* db = ctx.eval->vars;
    if (assign.var_kind == parser::VarRef::Kind::Node) {
        db = ctx.eval->node_vars;
    }

    if (!db) {
        return {};
    }

    switch (assign.op) {
    case parser::Assignment::Op::Set:
        db->set(*name, *value);
        break;
    case parser::Assignment::Op::Append:
        db->append(*name, *value);
        break;
    case parser::Assignment::Op::Define:
        // := means no further expansion
        db->set(*name, *value);
        break;
    case parser::Assignment::Op::SoftSet:
        // ?= - set only if variable is not already defined (first wins)
        if (!db->contains(*name)) {
            db->set(*name, *value);
        }
        break;
    case parser::Assignment::Op::WeakSet:
        // ??= - deferred assignment, applied at end of Tupfile (last wins)
        ctx.pending_weak_assignments.emplace_back(*name, *value);
        break;
    }

    return {};
}

auto GraphBuilder::process_conditional(
    BuilderContext& ctx,
    parser::Conditional const& cond
) -> Result<void>
{
    auto evaluator = parser::Evaluator { ctx.eval };
    auto condition_true = evaluator.evaluate_condition(cond);

    auto const& body = condition_true ? cond.then_body : cond.else_body;

    for (auto const& stmt : body) {
        auto result = Result<void> { process_statement(ctx, *stmt) };
        if (!result) {
            return pup::unexpected<Error>(result.error());
        }
    }

    return {};
}

auto GraphBuilder::process_include(
    BuilderContext& ctx,
    parser::Include const& inc
) -> Result<void>
{
    // Find the include file path
    auto include_path = std::string {};

    if (inc.is_rules) {
        // include_rules: search up directory tree for Tuprules.tup
        auto search_dir = fs::path { ctx.options.source_root / ctx.current_dir };
        auto root = fs::path { ctx.options.source_root };

        while (search_dir >= root) {
            auto tuprules = fs::path { search_dir / "Tuprules.tup" };
            if (fs::exists(tuprules)) {
                include_path = tuprules.string();
                break;
            }
            if (search_dir == root) {
                break;
            }
            search_dir = search_dir.parent_path();
        }

        if (include_path.empty()) {
            return {}; // No Tuprules.tup found, silently continue
        }
    } else {
        // include path: expand and resolve the path
        auto evaluator = parser::Evaluator { ctx.eval };
        auto path_result = Result<std::string> { evaluator.expand(inc.path) };
        if (!path_result) {
            return pup::unexpected<Error>(path_result.error());
        }

        auto resolved = fs::path { ctx.options.source_root / ctx.current_dir / *path_result };
        if (!fs::exists(resolved)) {
            return make_error<void>(ErrorCode::IncludeNotFound, "Include file not found: " + *path_result);
        }
        include_path = resolved.string();
    }

    // Prevent infinite recursion
    if (ctx.included_files.contains(include_path)) {
        return {};
    }
    ctx.included_files.insert(include_path);

    // Add included file to sticky_sources for dependency tracking
    auto inc_rel = fs::relative(include_path, ctx.options.source_root).string();
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
    auto parser = parser::Parser { source, include_path };
    auto parse_result = Result<parser::Tupfile> { parser.parse() };
    if (!parse_result) {
        for (auto const& err : parser.errors()) {
            fmt::print(stderr, "{}:{}:{}: error: {}\n", include_path, err.location.line, err.location.column, err.message);
        }
        return pup::unexpected<Error>(parse_result.error());
    }

    // For include_rules, temporarily set TUP_CWD to the relative path from
    // the Tupfile directory back to the Tuprules.tup directory. This allows
    // patterns like ROOT = $(TUP_CWD) to work correctly.
    auto old_tup_cwd = std::string {};
    if (inc.is_rules && ctx.eval) {
        old_tup_cwd = ctx.eval->tup_cwd;
        // Compute relative path from Tupfile directory to include file's directory
        auto include_dir = fs::path { include_path }.parent_path();
        auto rel_path = fs::relative(include_dir, ctx.options.source_root / ctx.current_dir);
        ctx.eval->tup_cwd = rel_path.empty() ? "." : rel_path.string();
    }

    // Process statements from the included file
    for (auto const& stmt : parse_result->statements) {
        auto result = Result<void> { process_statement(ctx, *stmt) };
        if (!result) {
            if (inc.is_rules && ctx.eval) {
                ctx.eval->tup_cwd = old_tup_cwd;
            }
            return pup::unexpected<Error>(result.error());
        }
    }

    // Restore original TUP_CWD
    if (inc.is_rules && ctx.eval) {
        ctx.eval->tup_cwd = old_tup_cwd;
    }

    return {};
}

auto GraphBuilder::process_import(
    BuilderContext& ctx,
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
    else if (auto it = impl_->options.cached_env_vars.find(imp.var_name);
             it != impl_->options.cached_env_vars.end()) {
        value = it->second;
    }
    // 3. Fall back to default value
    else if (imp.default_value) {
        auto evaluator = parser::Evaluator { ctx.eval };
        auto expanded = Result<std::string> { evaluator.expand(*imp.default_value) };
        if (!expanded) {
            return pup::unexpected<Error>(expanded.error());
        }
        value = *expanded;
    }
    // If no env, no cache, and no default, variable remains empty (tup behavior)

    // Create/update Variable node under $ directory for persistence
    if (impl_->env_var_dir_id != INVALID_NODE_ID) {
        auto node_name = imp.var_name + "=" + value;
        auto content_hash = sha256(value);

        // Check if we already have a node for this variable (from same build session)
        auto it = impl_->imported_env_var_nodes.find(imp.var_name);
        if (it != impl_->imported_env_var_nodes.end()) {
            // Update existing node in-place if value changed
            auto* existing = ctx.graph->get_node(it->second);
            if (existing && existing->name != node_name) {
                existing->name = node_name;
                existing->content_hash = content_hash;
            }
        } else {
            // Create new Variable node
            auto node = Node {
                .type = NodeType::Variable,
                .name = node_name,
                .parent_dir = impl_->env_var_dir_id,
                .content_hash = content_hash,
            };
            auto result = ctx.graph->add_node(std::move(node));
            if (result) {
                impl_->imported_env_var_nodes[imp.var_name] = *result;
            }
        }
    }

    if (ctx.vars) {
        ctx.vars->set(imp.var_name, value);
    }

    // Track this as an imported variable for fine-grained dependency tracking
    impl_->imported_var_names.insert(imp.var_name);

    return {};
}

auto GraphBuilder::process_export(
    BuilderContext& ctx,
    parser::Export const& exp
) -> Result<void>
{
    // Per tup manual: "adds the environment variable VARIABLE to the export
    // list for future :-rules"
    ctx.exported_vars.insert(exp.var_name);
    return {};
}

auto GraphBuilder::expand_rule(
    BuilderContext& ctx,
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
        cmd_inputs.push_back(transform_input_path(ctx, tc, inp));
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
    auto macro_name = std::string {};
    BangMacroDef const* macro_ptr = nullptr;

    {
        // Expand the command to detect if it's a macro reference
        auto expanded_cmd = Result<std::string> { expand_command(ctx, rule.command, flags, {}) };
        if (!expanded_cmd) {
            return pup::unexpected<Error>(expanded_cmd.error());
        }

        auto cmd_str = std::string { *expanded_cmd };
        // Trim whitespace
        while (!cmd_str.empty() && (cmd_str.front() == ' ' || cmd_str.front() == '\t')) {
            cmd_str.erase(0, 1);
        }

        if (!cmd_str.empty() && cmd_str[0] == '!') {
            // Bang macro reference - extract just the macro name (first word after !)
            auto name_end = cmd_str.find_first_of(" \t", 1);
            if (name_end == std::string::npos) {
                macro_name = cmd_str.substr(1);
            } else {
                macro_name = cmd_str.substr(1, name_end - 1);
            }

            auto it = decltype(ctx.macros)::iterator { ctx.macros.find(macro_name) };
            if (it == ctx.macros.end()) {
                return make_error<void>(ErrorCode::UnknownMacro, "Unknown bang macro: !" + macro_name);
            }

            macro_ptr = &it->second;
        }
    }

    // Pre-resolve order-only group references so %<group> can expand them in commands
    // This handles cross-directory groups like: | ../include/<gen-headers> |> cat %<gen-headers>
    auto rule_order_only_groups = std::unordered_map<std::string, std::vector<std::string>> {};
    auto evaluator = parser::Evaluator { ctx.eval };

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
                auto expanded = Result<std::string> { evaluator.expand(pattern.path) };
                if (expanded) {
                    group_dir = normalize_group_dir(*expanded, ctx.current_dir, ctx.options.source_root);
                }
            } else {
                group_dir = ctx.current_dir.empty() ? "." : ctx.current_dir.string();
            }

            // Demand-driven parsing: request the directory's Tupfile if not yet parsed
            auto dir_path = fs::path { group_dir };
            if (ctx.eval && ctx.eval->request_directory && ctx.eval->available_tupfile_dirs) {
                if (ctx.eval->available_tupfile_dirs->contains(dir_path)) {
                    (void)ctx.eval->request_directory(dir_path);
                }
            }

            // Get or create the Group node (groups are first-class nodes)
            auto group_id_result = get_or_create_group_node(ctx, group_dir, pattern.group_name);
            if (!group_id_result) {
                continue;
            }
            auto group_id = *group_id_result;

            // Get files that are members of this group (via file → group edges)
            auto members = get_group_members(*ctx.graph, group_id);
            if (!members.empty()) {
                if constexpr (DEBUG_OO_GROUPS) {
                    fmt::print(stderr, "[DEBUG OO GROUP] Lookup FOUND: <{}/{}> ({} members)\n", group_dir, pattern.group_name, members.size());
                }
                // Populate rule_order_only_groups for %<group> command expansion
                auto& paths = rule_order_only_groups[pattern.group_name];
                for (auto id : members) {
                    auto path = ctx.graph->get_full_path(id);
                    if (!path.empty()) {
                        paths.push_back(make_source_relative(path, tc.source_to_root, tc.current_dir_str));
                    }
                }
            } else {
                if constexpr (DEBUG_OO_GROUPS) {
                    fmt::print(stderr, "[DEBUG OO GROUP] Lookup DEFERRED: <{}/{}>\n", group_dir, pattern.group_name);
                }
            }
            // ALWAYS defer edge creation - the group might grow as more Tupfiles are parsed
            deferred_group_ids.insert(group_id);
        } else if (!pattern.path.empty()) {
            // Path expression that may contain <group> suffix: ../include/<gen-headers>
            auto expanded = Result<std::string> { evaluator.expand(pattern.path) };
            if (!expanded) {
                continue;
            }
            auto const& path = *expanded;
            auto lt_pos = path.rfind('<');
            auto gt_pos = path.rfind('>');
            if (lt_pos != std::string::npos && gt_pos != std::string::npos
                && gt_pos == path.size() - 1 && gt_pos > lt_pos) {
                auto group_name = path.substr(lt_pos + 1, gt_pos - lt_pos - 1);
                if (group_name.empty()) {
                    continue;
                }
                auto dir_part = path.substr(0, lt_pos);
                auto group_dir = normalize_group_dir(dir_part, ctx.current_dir, ctx.options.source_root);
                auto dir_path = fs::path { group_dir };

                // Demand-driven parsing: request the directory's Tupfile if not yet parsed
                if (ctx.eval && ctx.eval->request_directory && ctx.eval->available_tupfile_dirs) {
                    if (ctx.eval->available_tupfile_dirs->contains(dir_path)) {
                        // Note: Circular dependency errors are NOT fatal - the group may be
                        // registered after the current parsing context completes.
                        (void)ctx.eval->request_directory(dir_path);
                    }
                }

                // Get or create the Group node (groups are first-class nodes)
                auto group_id_result = get_or_create_group_node(ctx, group_dir, group_name);
                if (!group_id_result) {
                    continue;
                }
                auto group_id = *group_id_result;

                // Get files that are members of this group (via file → group edges)
                auto members = get_group_members(*ctx.graph, group_id);
                if (!members.empty()) {
                    if constexpr (DEBUG_OO_GROUPS) {
                        fmt::print(stderr, "[DEBUG OO GROUP] Path lookup FOUND: <{}/{}> ({} members) from {}\n", group_dir, group_name, members.size(), ctx.current_dir.string());
                    }
                    // Populate rule_order_only_groups for %<group> command expansion
                    auto& paths = rule_order_only_groups[group_name];
                    for (auto id : members) {
                        auto p = ctx.graph->get_full_path(id);
                        if (!p.empty()) {
                            paths.push_back(make_source_relative(p, tc.source_to_root, tc.current_dir_str));
                        }
                    }
                } else {
                    if constexpr (DEBUG_OO_GROUPS) {
                        fmt::print(stderr, "[DEBUG OO GROUP] Path lookup DEFERRED: <{}/{}> from {}\n", group_dir, group_name, ctx.current_dir.string());
                    }
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
    auto cmd_id = Result<NodeId> { create_command_node(ctx, cmd_text, display) };
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

    for (auto const& gen_rule : generated_rules) {
        auto gen_cmd_id = Result<NodeId> { create_command_node(ctx, gen_rule.command, gen_rule.display) };
        if (!gen_cmd_id) {
            continue;
        }

        // Create edges from inputs to generated command
        for (auto const& input : gen_rule.inputs) {
            auto input_id = Result<NodeId> { resolve_input_node(ctx, input) };
            if (input_id) {
                (void)ctx.graph->add_edge(*input_id, *gen_cmd_id);
            }
        }

        // Create order-only edges for generated command (e.g., gen-headers)
        // For group references, defer to resolve_deferred_order_only_edges()
        if constexpr (DEBUG_OO_GROUPS) {
            fmt::print(stderr, "[DEBUG OO GROUP] GenRule {} has {} order_only_inputs\n", gen_rule.command, gen_rule.order_only_inputs.size());
            for (auto const& oi : gen_rule.order_only_inputs) {
                fmt::print(stderr, "[DEBUG OO GROUP]   oi: '{}'\n", oi);
            }
        }
        for (auto const& oi : gen_rule.order_only_inputs) {
            // Check for path/<group> pattern
            auto lt_pos = oi.rfind('<');
            auto gt_pos = oi.rfind('>');
            if (lt_pos != std::string::npos && gt_pos != std::string::npos && gt_pos == oi.size() - 1 && gt_pos > lt_pos) {
                // This is a group reference - get/create group node and defer edge
                auto group_name = oi.substr(lt_pos + 1, gt_pos - lt_pos - 1);
                auto dir_part = oi.substr(0, lt_pos);
                auto group_dir = normalize_group_dir(dir_part, ctx.current_dir, ctx.options.source_root);
                auto group_id_result = get_or_create_group_node(ctx, group_dir, group_name);
                if (group_id_result) {
                    impl_->deferred_edges.insert({ *group_id_result, *gen_cmd_id });
                    if constexpr (DEBUG_OO_GROUPS) {
                        fmt::print(stderr, "[DEBUG OO GROUP] Generated cmd: deferred <{}/{}> (group_id={})\n", group_dir, group_name, *group_id_result);
                    }
                }
            } else if (!parser::has_glob_chars(oi)) {
                // Regular file path - create edge directly (skip glob patterns)
                auto oi_id = Result<NodeId> { resolve_input_node(ctx, oi) };
                if (oi_id) {
                    (void)ctx.graph->add_order_only_edge(*oi_id, *gen_cmd_id);
                }
            }
        }

        // Add edge from generated command to parent command (dep-scan runs before compile)
        (void)ctx.graph->add_edge(*gen_cmd_id, *cmd_id);

        // Store generated rule info on the node for scheduler to handle
        if (auto* node = ctx.graph->get_node(*gen_cmd_id)) {
            node->generated_output = gen_rule.outputs.empty() ? GeneratedOutput {} : gen_rule.outputs[0];
            node->output_action = gen_rule.action;
            node->parent_command = gen_rule.parent_command;
        }
    }

    // Create edges from inputs to command
    // Use file_inputs (excludes glob patterns which aren't valid paths)
    // Skip group references - they are handled by deferred edge resolution (order-only)
    for (auto const& input : file_inputs) {
        if (input.find('<') != std::string::npos && input.back() == '>') {
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
        auto* output_node = ctx.graph->get_node(*output_id);
        if (output_node && !output_node->inputs.empty()) {
            for (auto input_id : output_node->inputs) {
                if (is_command_id(input_id)) {
                    auto* existing_cmd = ctx.graph->get_node(input_id);
                    auto existing_cmd_str = existing_cmd ? existing_cmd->command : "<unknown>";
                    auto output_path = ctx.graph->get_full_path(*output_id);
                    return make_error<void>(
                        ErrorCode::DuplicateNode,
                        fmt::format("Unable to create output '{}' because it is already owned by command:\n  {}", output_path, existing_cmd_str)
                    );
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
                auto evaluator = parser::Evaluator { ctx.eval };
                auto expanded = evaluator.expand(*group_dir_expr);
                if (expanded) {
                    // Remove trailing slash and normalize
                    auto dir_path = std::string { *expanded };
                    while (!dir_path.empty() && dir_path.back() == '/') {
                        dir_path.pop_back();
                    }

                    // Resolve relative to current_dir
                    auto resolved = fs::path { ctx.current_dir } / dir_path;
                    dir = resolved.lexically_normal().string();
                }
            }

            if (dir.empty()) {
                dir = ctx.current_dir.empty() ? "." : ctx.current_dir.string();
            }

            // Create or get the Group node
            auto group_id_result = get_or_create_group_node(ctx, dir, *output_oo_group);
            if (group_id_result) {
                // Add edge: file → group (file is member of group)
                (void)ctx.graph->add_edge(*output_id, *group_id_result, LinkType::Group);
                if constexpr (DEBUG_OO_GROUPS) {
                    auto output_path = ctx.graph->get_full_path(*output_id);
                    fmt::print(stderr, "[DEBUG OO GROUP] Registered: <{}/{}> = {} (id={}) -> group {}\n", dir, *output_oo_group, output_path, *output_id, *group_id_result);
                }
            }
        }
    }

    // Create order-only edges from the pre-expanded paths
    // Skip group references (deferred edge creation) and glob patterns (not valid paths)
    for (auto const& oi : order_only_paths) {
        if (oi.find('<') != std::string::npos && oi.back() == '>') {
            continue;
        }
        if (parser::has_glob_chars(oi)) {
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
        impl_->deferred_edges.insert({ group_id, *cmd_id });
    }

    return {};
}

auto GraphBuilder::expand_inputs(
    BuilderContext& ctx,
    std::vector<parser::PathPattern> const& patterns
) -> Result<std::vector<std::string>>
{
    auto result = std::vector<std::string> {};
    auto evaluator = parser::Evaluator { ctx.eval };

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
            auto dir_path = fs::path {};

            if (!pattern.path.empty()) {
                auto expanded = Result<std::string> { evaluator.expand(pattern.path) };
                if (expanded) {
                    group_dir = normalize_group_dir(*expanded, ctx.current_dir, ctx.options.source_root);
                    dir_path = fs::path { group_dir };

                    // Demand-driven parsing: request the directory's Tupfile if not yet parsed
                    if (ctx.eval && ctx.eval->request_directory && ctx.eval->available_tupfile_dirs) {
                        if (ctx.eval->available_tupfile_dirs->contains(dir_path)) {
                            // Note: Circular dependency errors are NOT fatal - the group may be
                            // registered after the current parsing context completes.
                            (void)ctx.eval->request_directory(dir_path);
                        }
                    }
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
        auto paths = Result<std::vector<std::string>> { evaluator.expand_path(pattern) };
        if (!paths) {
            return pup::unexpected<Error>(paths.error());
        }

        for (auto& path : *paths) {
            // Check for path/<group> pattern (order-only group reference with directory prefix)
            // The expanded path will contain literal <groupname> suffix
            auto lt_pos = path.rfind('<');
            auto gt_pos = path.rfind('>');
            if (lt_pos != std::string::npos && gt_pos != std::string::npos && gt_pos == path.size() - 1 && gt_pos > lt_pos) {
                auto group_name = path.substr(lt_pos + 1, gt_pos - lt_pos - 1);
                if (group_name.empty()) {
                    continue; // Invalid empty group name
                }

                auto dir_part = path.substr(0, lt_pos);
                auto group_dir = normalize_group_dir(dir_part, ctx.current_dir, ctx.options.source_root);
                auto dir_path = fs::path { group_dir };

                // Demand-driven parsing: request the directory's Tupfile if not yet parsed
                if (ctx.eval && ctx.eval->request_directory && ctx.eval->available_tupfile_dirs) {
                    if (ctx.eval->available_tupfile_dirs->contains(dir_path)) {
                        // Note: Circular dependency errors are NOT fatal - the group may be
                        // registered after the current parsing context completes.
                        (void)ctx.eval->request_directory(dir_path);
                    }
                }

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
                auto base = std::filesystem::path { ctx.current_dir.empty() ? ctx.options.source_root
                                                                            : ctx.options.source_root / ctx.current_dir };

                // First try expanding against filesystem
                auto expanded = Result<std::vector<std::string>> { parser::glob_expand(path, base) };
                if (expanded && !expanded->empty()) {
                    for (auto& p : *expanded) {
                        // Prefix with current_dir to make path relative to project root
                        if (!ctx.current_dir.empty()) {
                            result.push_back((ctx.current_dir / p).string());
                        } else {
                            result.push_back(std::move(p));
                        }
                    }
                } else {
                    // No files on disk - look for matching Generated nodes in graph
                    // First, try demand-driven parsing of the directory containing the glob pattern
                    auto pattern_dir = fs::path { path }.parent_path();
                    auto abs_pattern_dir = fs::path { (ctx.current_dir / pattern_dir).lexically_normal() };
                    if (ctx.eval && ctx.eval->request_directory && ctx.eval->available_tupfile_dirs) {
                        if (ctx.eval->available_tupfile_dirs->contains(abs_pattern_dir)) {
                            // Note: Circular dependency errors are NOT fatal - the generated files
                            // may be registered after the current parsing context completes.
                            (void)ctx.eval->request_directory(abs_pattern_dir);
                        }
                    }

                    // Match glob pattern against Generated nodes (stored at source-root-relative paths)
                    auto pattern_path = ctx.current_dir.empty() ? path : (ctx.current_dir / path).lexically_normal().string();
                    auto glob = parser::Glob { pattern_path };
                    for (auto id : ctx.graph->nodes_of_type(NodeType::Generated)) {
                        auto node_path = ctx.graph->get_full_path(id);
                        if (!node_path.empty() && glob.matches(node_path)) {
                            result.push_back(std::move(node_path));
                        }
                    }
                }
            } else if (!parser::has_glob_chars(path)) {
                // Non-glob path: trigger demand-driven parsing if file doesn't exist
                // (path already added above, but we may need to request cross-directory Tupfile)
                auto full_path = std::filesystem::path { ctx.options.source_root / ctx.current_dir / path };
                if (!std::filesystem::exists(full_path)) {
                    auto file_dir = fs::path { path }.parent_path();
                    auto abs_file_dir = fs::path { (ctx.current_dir / file_dir).lexically_normal() };

                    if (ctx.eval && ctx.eval->request_directory && ctx.eval->available_tupfile_dirs) {
                        if (ctx.eval->available_tupfile_dirs->contains(abs_file_dir)) {
                            (void)ctx.eval->request_directory(abs_file_dir);
                        }
                    }
                }
            }
        }
    }

    // Handle exclusions (! for regular inputs, ^ for foreach exclusions)
    for (auto const& pattern : patterns) {
        if (!pattern.is_exclusion && !pattern.is_output_exclusion) {
            continue;
        }

        auto paths = Result<std::vector<std::string>> { evaluator.expand_path(pattern) };
        if (!paths) {
            continue;
        }

        for (auto const& excl : *paths) {
            // Expand globs in exclusion pattern if needed
            if (ctx.options.expand_globs && parser::has_glob_chars(excl)) {
                auto base = std::filesystem::path { ctx.current_dir.empty() ? ctx.options.source_root
                                                                            : ctx.options.source_root / ctx.current_dir };
                auto expanded = Result<std::vector<std::string>> { parser::glob_expand(excl, base) };
                if (expanded && !expanded->empty()) {
                    for (auto const& p : *expanded) {
                        // Normalize the same way as included paths
                        auto normalized = std::string {};
                        if (!ctx.current_dir.empty()) {
                            normalized = (ctx.current_dir / p).lexically_normal().string();
                        } else {
                            normalized = fs::path { p }.lexically_normal().string();
                        }

                        std::erase(result, normalized);
                    }
                }
            } else {
                // Non-glob exclusion: normalize path the same way as included paths
                auto normalized_excl = std::string {};
                if (!ctx.current_dir.empty()) {
                    normalized_excl = (ctx.current_dir / excl).lexically_normal().string();
                } else {
                    normalized_excl = fs::path { excl }.lexically_normal().string();
                }

                std::erase(result, normalized_excl);
            }
        }
    }

    return result;
}

auto GraphBuilder::expand_outputs(
    BuilderContext& ctx,
    std::vector<parser::PathPattern> const& patterns,
    parser::PatternFlags const& flags
) -> Result<std::vector<std::string>>
{
    auto result = std::vector<std::string> {};
    auto evaluator = parser::Evaluator { ctx.eval };

    for (auto const& pattern : patterns) {
        if (pattern.is_group) {
            continue; // Groups are not valid in outputs
        }
        if (pattern.is_output_exclusion) {
            continue; // Exclusion patterns are markers, not actual outputs
        }

        auto paths = Result<std::vector<std::string>> { evaluator.expand_path(pattern) };
        if (!paths) {
            return pup::unexpected<Error>(paths.error());
        }

        for (auto& path : *paths) {
            // Expand pattern flags (%B, %f, etc.)
            auto expanded = Result<std::string> { evaluator.expand_pattern(path, flags) };
            auto output_path = expanded ? *expanded : std::move(path);

            // Store at source-root-relative path (current_dir / output)
            // Variant mapping is applied only at I/O time (expand_command)
            if (!ctx.current_dir.empty()) {
                output_path = (ctx.current_dir / output_path).lexically_normal().string();
            }

            result.push_back(std::move(output_path));
        }
    }

    return result;
}

auto GraphBuilder::expand_command(
    BuilderContext& ctx,
    parser::Expression const& cmd,
    parser::PatternFlags flags,
    std::vector<std::string> const& outputs
) -> Result<std::string>
{
    auto evaluator = parser::Evaluator { ctx.eval };

    // Expand the command expression (variable expansion)
    auto literal = Result<std::string> { evaluator.expand(cmd) };
    if (!literal) {
        return pup::unexpected<Error>(literal.error());
    }

    auto expanded = Result<std::string> { evaluator.expand(std::string_view { *literal }) };
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
    return evaluator.expand_pattern(*expanded, flags);
}

namespace {
constexpr auto MAX_DIRECTORY_DEPTH = 128;
}

auto GraphBuilder::get_or_create_directory_node(
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
    auto node = Node {
        .type = NodeType::Directory,
        .name = basename,
        .parent_dir = parent_id,
    };

    return ctx.graph->add_node(std::move(node));
}

auto GraphBuilder::get_or_create_file_node(
    BuilderContext& ctx,
    std::string const& path,
    NodeType type
) -> Result<NodeId>
{
    // Convert working-directory-relative paths to source-root-relative or absolute
    // Paths like "../../build/foo" from "src/bar" should become "build/foo"
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
        if (type == NodeType::Generated) {
            auto* node = ctx.graph->get_node(*existing);
            if (node && (node->type == NodeType::Ghost || node->type == NodeType::File)) {
                // Ghost/File→Generated: upgrade type, preserve edges
                // Unlike Tup (which re-parses after upgrade), pup parses once
                // so existing edges from commands to this node are valid
                node->type = NodeType::Generated;
            }
        }
        return *existing;
    }

    // Create new node
    auto node = Node {
        .type = type,
        .name = basename,
        .parent_dir = parent_id,
    };

    return ctx.graph->add_node(std::move(node));
}

auto GraphBuilder::resolve_input_node(
    BuilderContext& ctx,
    std::string const& path
) -> Result<NodeId>
{
    // Unified source-root-relative storage:
    // All nodes (File, Ghost, Generated) are stored at source-root-relative paths.
    // Variant mapping is applied only at I/O time (expand_command, file operations).

    // Check if node already exists at this path (from earlier parsing)
    if (auto existing = ctx.graph->find_by_path(path)) {
        return *existing;
    }

    // Check if source file exists on disk
    auto source_path = ctx.options.source_root / path;
    if (fs::exists(source_path)) {
        return get_or_create_file_node(ctx, path, NodeType::File);
    }

    // For variant builds, also check if file exists only in variant directory
    // (e.g., tup.config, or files created during configure step)
    if (ctx.options.source_root != ctx.options.output_root) {
        auto variant_path = ctx.options.output_root / path;
        if (fs::exists(variant_path)) {
            return get_or_create_file_node(ctx, path, NodeType::File);
        }
    }

    // File doesn't exist - create Ghost at source-root-relative path
    // Ghost nodes are placeholders that will be upgraded to Generated
    // when the output rule is processed (same path, in-place upgrade)
    return get_or_create_file_node(ctx, path, NodeType::Ghost);
}

auto GraphBuilder::get_or_create_group_node(
    BuilderContext& ctx,
    std::string const& directory,
    std::string const& name
) -> Result<NodeId>
{
    // Check cache first (fast path)
    auto key = Impl::GroupKey { directory, name };
    auto it = impl_->group_nodes.find(key);
    if (it != impl_->group_nodes.end()) {
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
        impl_->group_nodes[key] = *existing;
        return *existing;
    }

    // Create new group node
    auto node = Node {
        .type = NodeType::Group,
        .name = group_basename,
        .parent_dir = parent_id,
    };

    auto result = ctx.graph->add_node(std::move(node));
    if (result) {
        impl_->group_nodes[key] = *result;
    }
    return result;
}

auto GraphBuilder::create_command_node(
    BuilderContext& ctx,
    std::string const& command,
    std::string const& display
) -> Result<NodeId>
{
    auto node = Node {
        .type = NodeType::Command,
        .command = command,
        .display = display,
        .source_dir = ctx.current_dir.string(),
        .exported_vars = ctx.exported_vars,
    };

    auto cmd_id_result = ctx.graph->add_node(std::move(node));
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
        auto it = impl_->config_var_nodes.find(var_name);
        if (it != impl_->config_var_nodes.end()) {
            (void)ctx.graph->add_edge(it->second, cmd_id, LinkType::Sticky);
        }
    }

    // Add sticky edges from used imported env variables (fine-grained dependency tracking)
    for (auto const& var_name : ctx.used_env_vars) {
        auto it = impl_->imported_env_var_nodes.find(var_name);
        if (it != impl_->imported_env_var_nodes.end()) {
            (void)ctx.graph->add_edge(it->second, cmd_id, LinkType::Sticky);
        }
    }

    return cmd_id;
}

auto GraphBuilder::resolve_deferred_order_only_edges(BuildGraph& graph) -> Result<void>
{
    if constexpr (DEBUG_OO_GROUPS) {
        fmt::print(stderr, "[DEBUG OO GROUP] Resolving {} deferred edges\n", impl_->deferred_edges.size());
    }
    // Resolve deferred order-only edges
    // With groups as first-class nodes, we create a single edge: group → command
    for (auto const& edge : impl_->deferred_edges) {
        // Verify group node exists and has members
        auto const* group_node = graph.get_node(edge.group_id);
        if (!group_node || group_node->type != NodeType::Group) {
            if constexpr (DEBUG_OO_GROUPS) {
                fmt::print(stderr, "[DEBUG OO GROUP] Deferred INVALID: group_id={} not a group\n", edge.group_id);
            }
            continue;
        }

        auto members = get_group_members(graph, edge.group_id);
        if (!members.empty()) {
            if constexpr (DEBUG_OO_GROUPS) {
                auto group_path = graph.get_full_path(edge.group_id);
                fmt::print(stderr, "[DEBUG OO GROUP] Deferred RESOLVED: {} -> cmd {} ({} members)\n", group_path, edge.command_id, members.size());
            }
            // Create single order-only edge: group → command
            (void)graph.add_order_only_edge(edge.group_id, edge.command_id);
        } else {
            if constexpr (DEBUG_OO_GROUPS) {
                auto group_path = graph.get_full_path(edge.group_id);
                fmt::print(stderr, "[DEBUG OO GROUP] Deferred EMPTY: {} has no members\n", group_path);
            }
            // Group exists but has no members - warn about potential typo
            auto group_path = graph.get_full_path(edge.group_id);
            impl_->warnings.push_back(fmt::format(
                "order-only group {} has no members",
                group_path
            ));
        }
    }

    // Clear deferred edges after resolution
    impl_->deferred_edges.clear();

    return {};
}

} // namespace pup::graph
