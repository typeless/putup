// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/graph/builder.hpp"
#include "pup/graph/rule_pattern.hpp"
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

namespace {

/// Strip trailing slashes from a path string
auto strip_trailing_slashes(std::string str) -> std::string
{
    while (!str.empty() && (str.back() == '/' || str.back() == '\\'))
        str.pop_back();
    return str;
}

/// Find a node by walking path components using find_by_dir_name
auto find_node_by_path(BuildGraph const& graph, std::string_view path) -> std::optional<NodeId>
{
    if (path.empty())
        return std::nullopt;

    auto p = fs::path { path };
    auto parent_id = NodeId { 0 };

    for (auto const& component : p) {
        auto name = component.string();
        if (name.empty() || name == ".")
            continue;

        auto found = graph.find_by_dir_name(parent_id, name);
        if (!found)
            return std::nullopt;

        parent_id = *found;
    }

    return parent_id != 0 ? std::optional { parent_id } : std::nullopt;
}

/// Normalize a file path for consistent lookup
/// - Removes double slashes
/// - Resolves . and .. components using lexically_normal
auto normalize_path(std::string const& path_str) -> std::string
{
    if (path_str.empty())
        return path_str;
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
    fs::path const& source_root) -> std::string
{
    auto cleaned = strip_trailing_slashes(path_str);
    auto path = fs::path { cleaned }.lexically_normal();

    if (path.is_absolute())
        path = fs::relative(path, source_root);
    else if (!current_dir.empty() && !path.empty()) {
        // Only combine with current_dir if path needs parent resolution (starts with ..)
        // Paths like $(ROOT)/foo expand to root-relative and should NOT be combined
        auto first = *path.begin();
        if (first == "..")
            path = (current_dir / path).lexically_normal();
    }

    return path.empty() ? "." : path.string();
}

/// Map an output path to the output directory.
/// All paths are project-relative (tup-style), never absolute.
/// For in-tree builds: variant_dir/current_dir/path (e.g., "build/src/lib/foo.o")
/// For out-of-tree builds (-B): variant_dir/current_dir/path (e.g., "build-s1f3/boot/boot.hex")
auto map_to_variant(
    std::string const& path,
    std::filesystem::path const& current_dir,
    std::filesystem::path const& variant_dir,
    std::filesystem::path const& source_root,
    std::filesystem::path const& output_root) -> std::string
{
    auto p = std::filesystem::path { path };

    // If path is already absolute, make it relative to source_root
    if (p.is_absolute()) {
        auto rel = fs::relative(p, source_root);
        if (!rel.empty() && rel.string()[0] != '.')
            return rel.lexically_normal().string();
        // Can't make relative - return as-is (shouldn't happen in normal use)
        return p.lexically_normal().string();
    }

    // Out-of-tree build (-B): use variant_dir which is the output directory basename
    // relative to source root (e.g., "build-s1f3" for -B build-s1f3)
    if (!output_root.empty() && source_root != output_root) {
        // Compute variant_dir from output_root if not explicitly provided
        auto effective_variant = variant_dir.empty()
            ? fs::relative(output_root, source_root)
            : variant_dir;
        if (current_dir.empty())
            return (effective_variant / path).lexically_normal().string();
        return (effective_variant / current_dir / path).lexically_normal().string();
    }

    // In-tree build: paths are project-relative
    if (variant_dir.empty()) {
        if (current_dir.empty())
            return path;
        return (current_dir / path).lexically_normal().string();
    }

    auto full_path = std::filesystem::path { variant_dir / current_dir / path };
    return full_path.lexically_normal().string();
}

} // namespace

GraphBuilder::GraphBuilder(BuilderOptions options)
    : options_(std::move(options))
{
}

auto GraphBuilder::build(parser::Tupfile const& tupfile, parser::EvalContext& eval)
    -> Result<BuildGraph>
{
    auto graph = BuildGraph {};
    auto result = Result<void> { add_tupfile(graph, tupfile, eval) };
    if (!result)
        return pup::unexpected<Error>(result.error());
    return graph;
}

auto GraphBuilder::add_tupfile(
    BuildGraph& graph,
    parser::Tupfile const& tupfile,
    parser::EvalContext& eval) -> Result<void>
{
    // Compute current_dir relative to source_root
    auto tupfile_parent = std::filesystem::path { tupfile.filename }.parent_path();
    auto relative_dir = std::filesystem::relative(tupfile_parent, options_.source_root);
    if (relative_dir == ".")
        relative_dir = "";

    auto ctx = BuilderContext {
        .graph = &graph,
        .eval = &eval,
        .vars = eval.vars,
        .options = options_,
        .current_dir = relative_dir,
        .current_file = tupfile.filename,
    };

    // Create Tupfile node and add to sticky_sources for dependency tracking
    auto tupfile_rel = std::filesystem::relative(tupfile.filename, options_.source_root).string();
    auto tupfile_node_result = get_or_create_file_node(ctx, tupfile_rel, NodeType::File);
    if (tupfile_node_result)
        ctx.sticky_sources.push_back(*tupfile_node_result);

    // Set up resolve_group callback for {group} pattern expansion
    eval.resolve_group = [&ctx](std::string_view name) -> std::vector<std::string> {
        auto it = ctx.groups.find(std::string { name });
        if (it == ctx.groups.end())
            return {};
        auto paths = std::vector<std::string> {};
        for (auto id : it->second) {
            auto path = ctx.graph->get_full_path(id);
            if (!path.empty())
                paths.push_back(std::move(path));
        }
        return paths;
    };

    // Set up resolve_order_only_group callback for %<group> pattern expansion in commands
    // This is for local group references (no directory prefix) - uses current directory
    eval.resolve_order_only_group = [this, &ctx](std::string_view name) -> std::vector<std::string> {
        auto dir = ctx.current_dir.empty() ? "." : ctx.current_dir.string();
        auto key = GroupKey { dir, std::string { name } };
        auto it = order_only_groups_.find(key);
        if (it == order_only_groups_.end())
            return {};
        auto paths = std::vector<std::string> {};
        for (auto id : it->second) {
            auto path = ctx.graph->get_full_path(id);
            if (!path.empty())
                paths.push_back(std::move(path));
        }
        return paths;
    };

    for (auto const& stmt : tupfile.statements) {
        auto result = Result<void> { process_statement(ctx, *stmt) };
        if (!result) {
            errors_.push_back(result.error().message);
            if (!options_.verbose)
                return pup::unexpected<Error>(result.error());
        }
    }

    // Copy errors and warnings
    for (auto& err : ctx.errors)
        errors_.push_back(std::move(err));
    for (auto& warn : ctx.warnings)
        warnings_.push_back(std::move(warn));

    return {};
}

auto GraphBuilder::process_statement(
    BuilderContext& ctx,
    parser::Statement const& stmt) -> Result<void>
{
    if (auto const* rule = stmt.as<parser::Rule>())
        return process_rule(ctx, *rule);

    if (auto const* macro = stmt.as<parser::BangMacro>())
        return process_bang_macro(ctx, *macro);

    if (auto const* assign = stmt.as<parser::Assignment>())
        return process_assignment(ctx, *assign);

    if (auto const* cond = stmt.as<parser::Conditional>())
        return process_conditional(ctx, *cond);

    if (auto const* inc = stmt.as<parser::Include>())
        return process_include(ctx, *inc);

    if (auto const* imp = stmt.as<parser::Import>())
        return process_import(ctx, *imp);

    if (auto const* exp = stmt.as<parser::Export>())
        return process_export(ctx, *exp);

    // Other directives (preload, run, error) not yet implemented
    return {};
}

auto GraphBuilder::process_rule(
    BuilderContext& ctx,
    parser::Rule const& rule) -> Result<void>
{
    // Expand input patterns
    auto inputs = Result<std::vector<std::string>> { expand_inputs(ctx, rule.inputs) };
    if (!inputs)
        return pup::unexpected<Error>(inputs.error());

    if (rule.foreach_) {
        // Foreach rule: create one command per input
        for (auto const& input : *inputs) {
            auto result = Result<void> { expand_rule(ctx, rule, { input }) };
            if (!result)
                return pup::unexpected<Error>(result.error());
        }
    } else {
        // Normal rule: single command for all inputs
        auto result = Result<void> { expand_rule(ctx, rule, *inputs) };
        if (!result)
            return pup::unexpected<Error>(result.error());
    }

    return {};
}

auto GraphBuilder::process_bang_macro(
    BuilderContext& ctx,
    parser::BangMacro const& macro) -> Result<void>
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
    parser::Assignment const& assign) -> Result<void>
{
    auto evaluator = parser::Evaluator { *ctx.eval };

    // Evaluate the variable name (may contain variable refs like foo-$(BAR))
    auto name = Result<std::string> { evaluator.expand(assign.name) };
    if (!name)
        return pup::unexpected<Error>(name.error());

    // Evaluate the value
    auto value = Result<std::string> { evaluator.expand(assign.value) };
    if (!value)
        return pup::unexpected<Error>(value.error());

    auto* db = ctx.eval->vars;
    if (assign.var_kind == parser::VarRef::Kind::Config)
        db = ctx.eval->config_vars;
    else if (assign.var_kind == parser::VarRef::Kind::Node)
        db = ctx.eval->node_vars;

    if (!db)
        return {};

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
    }

    return {};
}

auto GraphBuilder::process_conditional(
    BuilderContext& ctx,
    parser::Conditional const& cond) -> Result<void>
{
    auto evaluator = parser::Evaluator { *ctx.eval };
    auto condition_true = evaluator.evaluate_condition(cond);

    auto const& body = condition_true ? cond.then_body : cond.else_body;

    for (auto const& stmt : body) {
        auto result = Result<void> { process_statement(ctx, *stmt) };
        if (!result)
            return pup::unexpected<Error>(result.error());
    }

    return {};
}

auto GraphBuilder::process_include(
    BuilderContext& ctx,
    parser::Include const& inc) -> Result<void>
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
            if (search_dir == root)
                break;
            search_dir = search_dir.parent_path();
        }

        if (include_path.empty())
            return {}; // No Tuprules.tup found, silently continue
    } else {
        // include path: expand and resolve the path
        auto evaluator = parser::Evaluator { *ctx.eval };
        auto path_result = Result<std::string> { evaluator.expand(inc.path) };
        if (!path_result)
            return pup::unexpected<Error>(path_result.error());

        auto resolved = fs::path { ctx.options.source_root / ctx.current_dir / *path_result };
        if (!fs::exists(resolved))
            return make_error<void>(ErrorCode::IncludeNotFound,
                "Include file not found: " + *path_result);
        include_path = resolved.string();
    }

    // Prevent infinite recursion
    if (ctx.included_files.contains(include_path))
        return {};
    ctx.included_files.insert(include_path);

    // Add included file to sticky_sources for dependency tracking
    auto inc_rel = fs::relative(include_path, ctx.options.source_root).string();
    auto inc_node_result = get_or_create_file_node(ctx, inc_rel, NodeType::File);
    if (inc_node_result)
        ctx.sticky_sources.push_back(*inc_node_result);

    // Read the include file
    auto file = std::ifstream { include_path };
    if (!file)
        return make_error<void>(ErrorCode::IoError,
            "Cannot open include file: " + include_path);

    auto ss = std::stringstream {};
    ss << file.rdbuf();
    auto source = std::string { ss.str() };

    // Parse the include file
    auto parser = parser::Parser { source, include_path };
    auto parse_result = Result<parser::Tupfile> { parser.parse() };
    if (!parse_result)
        return pup::unexpected<Error>(parse_result.error());

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
            if (inc.is_rules && ctx.eval)
                ctx.eval->tup_cwd = old_tup_cwd;
            return pup::unexpected<Error>(result.error());
        }
    }

    // Restore original TUP_CWD
    if (inc.is_rules && ctx.eval)
        ctx.eval->tup_cwd = old_tup_cwd;

    return {};
}

auto GraphBuilder::process_import(
    BuilderContext& ctx,
    parser::Import const& imp) -> Result<void>
{
    // Per tup manual: "sets a variable inside the Tupfile that has the value
    // of the environment variable"
    auto value = std::string {};

    // Try environment first
    if (auto const* env_val = std::getenv(imp.var_name.c_str()))
        value = env_val;
    else if (imp.default_value) {
        // Expand default value expression
        auto evaluator = parser::Evaluator { *ctx.eval };
        auto expanded = Result<std::string> { evaluator.expand(*imp.default_value) };
        if (!expanded)
            return pup::unexpected<Error>(expanded.error());
        value = *expanded;
    }
    // If no env and no default, variable remains empty (tup behavior)

    if (ctx.vars)
        ctx.vars->set(imp.var_name, value);

    return {};
}

auto GraphBuilder::process_export(
    BuilderContext& ctx,
    parser::Export const& exp) -> Result<void>
{
    // Per tup manual: "adds the environment variable VARIABLE to the export
    // list for future :-rules"
    ctx.exported_vars.insert(exp.var_name);
    return {};
}

auto GraphBuilder::expand_rule(
    BuilderContext& ctx,
    parser::Rule const& rule,
    std::vector<std::string> const& inputs) -> Result<void>
{
    // Get the primary input for pattern expansion
    auto primary_input = inputs.empty() ? std::string {} : inputs[0];

    // Check if command is a bang macro reference
    auto cmd_text = std::string {};
    auto display = std::string {};
    auto outputs_patterns = rule.outputs;
    auto macro_name = std::string {};
    BangMacroDef const* macro_ptr = nullptr;

    // First expand the command to see if it's a macro reference
    auto expanded_cmd = Result<std::string> { expand_command(ctx, rule.command, inputs, {}) };
    if (!expanded_cmd)
        return pup::unexpected<Error>(expanded_cmd.error());

    auto cmd_str = std::string { *expanded_cmd };
    // Trim whitespace
    while (!cmd_str.empty() && (cmd_str.front() == ' ' || cmd_str.front() == '\t'))
        cmd_str.erase(0, 1);

    if (!cmd_str.empty() && cmd_str[0] == '!') {
        // Bang macro reference - extract just the macro name (first word after !)
        auto name_end = cmd_str.find_first_of(" \t", 1);
        if (name_end == std::string::npos)
            macro_name = cmd_str.substr(1);
        else
            macro_name = cmd_str.substr(1, name_end - 1);

        auto it = decltype(ctx.macros)::iterator { ctx.macros.find(macro_name) };
        if (it == ctx.macros.end())
            return make_error<void>(ErrorCode::UnknownMacro,
                "Unknown bang macro: !" + macro_name);

        macro_ptr = &it->second;

        // Use macro's outputs if rule doesn't specify any
        if (outputs_patterns.empty())
            outputs_patterns = macro_ptr->outputs;
    }

    // Expand outputs
    auto outputs = Result<std::vector<std::string>> { expand_outputs(ctx, outputs_patterns, primary_input) };
    if (!outputs)
        return pup::unexpected<Error>(outputs.error());

    // Now expand command with actual outputs for %o substitution
    if (macro_ptr) {
        auto macro_cmd = Result<std::string> { expand_command(ctx, macro_ptr->command, inputs, *outputs) };
        if (!macro_cmd)
            return pup::unexpected<Error>(macro_cmd.error());
        cmd_text = *macro_cmd;

        if (macro_ptr->display) {
            auto disp_result = Result<std::string> { expand_command(ctx, *macro_ptr->display, inputs, *outputs) };
            if (disp_result)
                display = *disp_result;
        }
    } else {
        auto full_cmd = Result<std::string> { expand_command(ctx, rule.command, inputs, *outputs) };
        if (!full_cmd)
            return pup::unexpected<Error>(full_cmd.error());
        cmd_text = *full_cmd;

        if (rule.display) {
            auto disp_result = Result<std::string> { expand_command(ctx, *rule.display, inputs, *outputs) };
            if (disp_result)
                display = *disp_result;
        }
    }

    // Expand order-only inputs early so we can pass them to generated rules
    auto all_order_only = rule.order_only_inputs;
    if (macro_ptr && !macro_ptr->order_only_inputs.empty()) {
        all_order_only.insert(all_order_only.end(),
            macro_ptr->order_only_inputs.begin(),
            macro_ptr->order_only_inputs.end());
    }
    auto order_only_paths = std::vector<std::string> {};
    for (auto const& pattern : all_order_only) {
        auto order_inputs = Result<std::vector<std::string>> { expand_inputs(ctx, { pattern }) };
        if (order_inputs) {
            order_only_paths.insert(order_only_paths.end(),
                order_inputs->begin(), order_inputs->end());
        }
    }

    // Create command node
    auto cmd_id = Result<NodeId> { create_command_node(ctx, cmd_text, display) };
    if (!cmd_id)
        return pup::unexpected<Error>(cmd_id.error());

    // Check for pattern matches and generate additional rules
    if (ctx.options.pattern_registry && !ctx.options.pattern_registry->empty()) {
        auto cmd_info = CommandInfo {
            .node_id = *cmd_id,
            .command = cmd_text,
            .display = display,
            .inputs = inputs,
            .order_only_inputs = order_only_paths,
            .outputs = *outputs,
            .working_dir = ctx.current_dir.string(),
        };
        auto generated_rules = ctx.options.pattern_registry->match_and_generate(cmd_info);
        for (auto const& gen_rule : generated_rules) {
            auto gen_cmd_id = Result<NodeId> { create_command_node(ctx, gen_rule.command, gen_rule.display) };
            if (!gen_cmd_id)
                continue;

            // Create edges from inputs to generated command
            for (auto const& input : gen_rule.inputs) {
                auto input_id = Result<NodeId> { get_or_create_file_node(ctx, input, NodeType::File) };
                if (input_id)
                    (void)ctx.graph->add_edge(*input_id, *gen_cmd_id);
            }

            // Create order-only edges for generated command (e.g., gen-headers)
            for (auto const& oi : gen_rule.order_only_inputs) {
                auto oi_id = Result<NodeId> { get_or_create_file_node(ctx, oi, NodeType::File) };
                if (oi_id)
                    (void)ctx.graph->add_order_only_edge(*oi_id, *gen_cmd_id);
            }

            // Add edge from generated command to parent command (dep-scan runs before compile)
            (void)ctx.graph->add_edge(*gen_cmd_id, *cmd_id);

            // Store generated rule info on the node for scheduler to handle
            if (auto* node = ctx.graph->get_node_mut(*gen_cmd_id)) {
                node->generated_output = gen_rule.outputs.empty() ? GeneratedOutput {} : gen_rule.outputs[0];
                node->output_action = gen_rule.action;
                node->parent_command = gen_rule.parent_command;
            }
        }
    }

    // Create edges from inputs to command
    for (auto const& input : inputs) {
        auto input_id = Result<NodeId> { get_or_create_file_node(ctx, input, NodeType::File) };
        if (!input_id)
            return pup::unexpected<Error>(input_id.error());
        auto edge_result = Result<void> { ctx.graph->add_edge(*input_id, *cmd_id) };
        if (!edge_result)
            return pup::unexpected<Error>(edge_result.error());
    }

    // Create edges from command to outputs
    for (auto const& output : *outputs) {
        auto output_id = Result<NodeId> { get_or_create_file_node(ctx, output, NodeType::Generated) };
        if (!output_id)
            return pup::unexpected<Error>(output_id.error());
        auto edge_result = Result<void> { ctx.graph->add_edge(*cmd_id, *output_id) };
        if (!edge_result)
            return pup::unexpected<Error>(edge_result.error());

        // Add to output group {name} if specified
        auto output_group = rule.output_group;
        if (!output_group && macro_ptr && macro_ptr->output_group)
            output_group = macro_ptr->output_group;
        if (output_group)
            ctx.groups[*output_group].push_back(*output_id);

        // Add to order-only group <name> if specified
        // Supports path/<group> syntax where path specifies the group's directory
        auto output_oo_group = rule.output_order_only_group;
        if (!output_oo_group && macro_ptr && macro_ptr->output_order_only_group)
            output_oo_group = macro_ptr->output_order_only_group;
        if (output_oo_group) {
            auto dir = std::string {};

            // Get directory from path prefix if specified
            auto const* group_dir_expr = rule.output_order_only_group_dir
                ? &*rule.output_order_only_group_dir
                : (macro_ptr && macro_ptr->output_order_only_group_dir
                          ? &*macro_ptr->output_order_only_group_dir
                          : nullptr);

            if (group_dir_expr) {
                auto evaluator = parser::Evaluator { *ctx.eval };
                auto expanded = evaluator.expand(*group_dir_expr);
                if (expanded) {
                    // Remove trailing slash and normalize
                    auto dir_path = std::string { *expanded };
                    while (!dir_path.empty() && dir_path.back() == '/')
                        dir_path.pop_back();

                    // Resolve relative to current_dir
                    auto resolved = fs::path { ctx.current_dir } / dir_path;
                    dir = resolved.lexically_normal().string();
                }
            }

            if (dir.empty())
                dir = ctx.current_dir.empty() ? "." : ctx.current_dir.string();

            auto key = GroupKey { dir, *output_oo_group };
            order_only_groups_[key].push_back(*output_id);
        }
    }

    // Create order-only edges from the pre-expanded paths
    for (auto const& oi : order_only_paths) {
        auto oi_id = Result<NodeId> { get_or_create_file_node(ctx, oi, NodeType::File) };
        if (oi_id)
            (void)ctx.graph->add_order_only_edge(*oi_id, *cmd_id);
    }

    return {};
}

auto GraphBuilder::expand_inputs(
    BuilderContext& ctx,
    std::vector<parser::PathPattern> const& patterns) -> Result<std::vector<std::string>>
{
    auto result = std::vector<std::string> {};
    auto evaluator = parser::Evaluator { *ctx.eval };

    for (auto const& pattern : patterns) {
        if (pattern.is_exclusion)
            continue; // Handle exclusions later

        if (pattern.is_group) {
            // Bin reference {name} - local to Tupfile
            auto it = decltype(ctx.groups)::iterator { ctx.groups.find(pattern.group_name) };
            if (it != ctx.groups.end()) {
                for (auto id : it->second) {
                    auto path = ctx.graph->get_full_path(id);
                    if (!path.empty())
                        result.push_back(std::move(path));
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
                            auto req_result = Result<void> { ctx.eval->request_directory(dir_path) };
                            if (!req_result)
                                return pup::unexpected<Error>(req_result.error());
                        }
                    }
                }
            } else {
                group_dir = ctx.current_dir.empty() ? "." : ctx.current_dir.string();
            }

            auto key = GroupKey { group_dir, pattern.group_name };
            auto it = order_only_groups_.find(key);
            if (it != order_only_groups_.end()) {
                for (auto id : it->second) {
                    auto path = ctx.graph->get_full_path(id);
                    if (!path.empty())
                        result.push_back(std::move(path));
                }
            }
            continue;
        }

        // Expand path expression
        auto paths = Result<std::vector<std::string>> { evaluator.expand_path(pattern) };
        if (!paths)
            return pup::unexpected<Error>(paths.error());

        for (auto& path : *paths) {
            // Check for path/<group> pattern (order-only group reference with directory prefix)
            // The expanded path will contain literal <groupname> suffix
            auto lt_pos = path.rfind('<');
            auto gt_pos = path.rfind('>');
            if (lt_pos != std::string::npos && gt_pos != std::string::npos && gt_pos == path.size() - 1 && gt_pos > lt_pos) {
                auto group_name = path.substr(lt_pos + 1, gt_pos - lt_pos - 1);
                if (group_name.empty())
                    continue; // Invalid empty group name

                auto dir_part = path.substr(0, lt_pos);
                auto group_dir = normalize_group_dir(dir_part, ctx.current_dir, ctx.options.source_root);
                auto dir_path = fs::path { group_dir };

                // Demand-driven parsing: request the directory's Tupfile if not yet parsed
                if (ctx.eval && ctx.eval->request_directory && ctx.eval->available_tupfile_dirs) {
                    if (ctx.eval->available_tupfile_dirs->contains(dir_path)) {
                        auto req_result = Result<void> { ctx.eval->request_directory(dir_path) };
                        if (!req_result)
                            return pup::unexpected<Error>(req_result.error());
                    }
                }

                // Look up the group
                auto key = GroupKey { group_dir, group_name };
                auto it = order_only_groups_.find(key);
                if (it != order_only_groups_.end()) {
                    for (auto id : it->second) {
                        auto path = ctx.graph->get_full_path(id);
                        if (!path.empty())
                            result.push_back(std::move(path));
                    }
                }
                continue;
            }
            // Expand globs if enabled
            if (ctx.options.expand_globs && parser::has_glob_chars(path)) {
                auto base = std::filesystem::path { ctx.current_dir.empty() ? ctx.options.source_root
                                                                            : ctx.options.source_root / ctx.current_dir };

                // First try expanding against filesystem
                auto expanded = Result<std::vector<std::string>> { parser::glob_expand(path, base) };
                if (expanded && !expanded->empty()) {
                    for (auto& p : *expanded) {
                        // Prefix with current_dir to make path relative to project root
                        if (!ctx.current_dir.empty())
                            result.push_back((ctx.current_dir / p).string());
                        else
                            result.push_back(std::move(p));
                    }
                } else {
                    // No files on disk - look for matching Generated nodes in graph
                    // First, try demand-driven parsing of the directory containing the glob pattern
                    auto pattern_dir = fs::path { path }.parent_path();
                    auto abs_pattern_dir = fs::path { (ctx.current_dir / pattern_dir).lexically_normal() };
                    if (ctx.eval && ctx.eval->request_directory && ctx.eval->available_tupfile_dirs) {
                        if (ctx.eval->available_tupfile_dirs->contains(abs_pattern_dir)) {
                            auto req_result = Result<void> { ctx.eval->request_directory(abs_pattern_dir) };
                            if (!req_result)
                                return pup::unexpected<Error>(req_result.error());
                        }
                    }

                    // Map the pattern to variant path for matching
                    auto variant_path = map_to_variant(path, ctx.current_dir, ctx.options.variant_dir,
                        ctx.options.source_root, ctx.options.output_root);
                    auto glob = parser::Glob { variant_path };
                    for (auto id : ctx.graph->nodes_of_type(NodeType::Generated)) {
                        auto node_path = ctx.graph->get_full_path(id);
                        if (!node_path.empty() && glob.matches(node_path))
                            result.push_back(std::move(node_path));
                    }
                }
            } else {
                // Non-glob path: check if file exists on disk, or find in graph
                auto full_path = std::filesystem::path { ctx.options.source_root / ctx.current_dir / path };
                if (std::filesystem::exists(full_path)) {
                    // Resolve path relative to current_dir and normalize to project-root-relative
                    // This handles paths like "../../include/foo.h" from "modules/kernel"
                    // -> "modules/kernel/../../include/foo.h" -> "include/foo.h"
                    if (!ctx.current_dir.empty()) {
                        auto resolved = (ctx.current_dir / path).lexically_normal();
                        result.push_back(resolved.string());
                    } else {
                        result.push_back(std::move(path));
                    }
                } else if (full_path.filename() == "tup.config" && (!ctx.options.variant_dir.empty() || !ctx.options.output_root.empty())) {
                    // Special case: tup.config lives in variant/output directory, not source root
                    // For -B builds: check output_root/tup.config
                    // For --variant builds: check source_root/variant_dir/tup.config
                    auto config_found = false;
                    if (!ctx.options.output_root.empty()) {
                        auto out_config = ctx.options.output_root / "tup.config";
                        if (fs::exists(out_config)) {
                            // Return relative path from SOURCE working dir to OUTPUT tup.config
                            // Using TUP_VARIANT_OUTPUTDIR-style path that reaches from source to output
                            // NOTE: We manually compute the relative path to avoid fs::relative()
                            // which resolves symlinks (tup.config is often a symlink to configs/*.config)
                            auto src_dir = ctx.options.source_root / ctx.current_dir;
                            auto rel_to_root = fs::relative(ctx.options.source_root, src_dir);
                            auto rel_output = fs::relative(ctx.options.output_root, ctx.options.source_root);
                            auto rel = rel_to_root / rel_output / "tup.config";
                            result.push_back(rel.lexically_normal().string());
                            config_found = true;
                        }
                    }
                    if (!config_found && !ctx.options.variant_dir.empty()) {
                        auto variant_config = ctx.options.variant_dir / "tup.config";
                        if (fs::exists(ctx.options.source_root / variant_config)) {
                            result.push_back(variant_config.string());
                            config_found = true;
                        }
                    }
                    if (!config_found)
                        result.push_back(std::move(path));
                } else {
                    // Not on disk - try demand-driven parsing of the file's directory
                    auto file_dir = fs::path { path }.parent_path();
                    auto abs_file_dir = fs::path { (ctx.current_dir / file_dir).lexically_normal() };

                    // If path references the variant output, map back to source for Tupfile lookup
                    // E.g., "../../build-s1f3/modules/kernel" -> "modules/kernel"
                    auto source_dir = abs_file_dir;
                    auto variant_prefix = ctx.options.variant_dir.string();
                    if (!variant_prefix.empty()) {
                        auto abs_dir_str = abs_file_dir.string();
                        if (abs_dir_str.starts_with(variant_prefix + "/")) {
                            source_dir = fs::path { abs_dir_str.substr(variant_prefix.size() + 1) };
                        } else if (abs_dir_str == variant_prefix) {
                            source_dir = fs::path { "." };
                        }
                    }

                    if (ctx.eval && ctx.eval->request_directory && ctx.eval->available_tupfile_dirs) {
                        if (ctx.eval->available_tupfile_dirs->contains(source_dir)) {
                            auto req_result = Result<void> { ctx.eval->request_directory(source_dir) };
                            if (!req_result)
                                return pup::unexpected<Error>(req_result.error());
                        }
                    }

                    // Check for generated file in variant
                    // Try multiple path formats in order of likelihood
                    auto abs_path = full_path.lexically_normal().string();
                    auto rel_path = fs::path { abs_path }.lexically_relative(ctx.options.source_root).string();

                    // First try absolute path (outputs from -B builds use absolute paths)
                    if (find_node_by_path(*ctx.graph, abs_path)) {
                        result.push_back(abs_path);
                    }
                    // Then try project-relative path
                    else if (find_node_by_path(*ctx.graph, rel_path)) {
                        result.push_back(rel_path);
                    }
                    // Try mapping to variant (for simple paths like "foo.o")
                    else {
                        auto variant_path = map_to_variant(path, ctx.current_dir, ctx.options.variant_dir,
                            ctx.options.source_root, ctx.options.output_root);
                        if (find_node_by_path(*ctx.graph, variant_path)) {
                            result.push_back(variant_path);
                        } else {
                            // Fall back to project-relative path for error messages
                            result.push_back(rel_path);
                        }
                    }
                }
            }
        }
    }

    // Handle exclusions
    for (auto const& pattern : patterns) {
        if (!pattern.is_exclusion)
            continue;

        auto paths = Result<std::vector<std::string>> { evaluator.expand_path(pattern) };
        if (!paths)
            continue;

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
                        if (!ctx.current_dir.empty())
                            normalized = (ctx.current_dir / p).lexically_normal().string();
                        else
                            normalized = fs::path { p }.lexically_normal().string();

                        result.erase(
                            std::remove(result.begin(), result.end(), normalized),
                            result.end());
                    }
                }
            } else {
                // Non-glob exclusion: normalize path the same way as included paths
                auto normalized_excl = std::string {};
                if (!ctx.current_dir.empty())
                    normalized_excl = (ctx.current_dir / excl).lexically_normal().string();
                else
                    normalized_excl = fs::path { excl }.lexically_normal().string();

                result.erase(
                    std::remove(result.begin(), result.end(), normalized_excl),
                    result.end());
            }
        }
    }

    return result;
}

auto GraphBuilder::expand_outputs(
    BuilderContext& ctx,
    std::vector<parser::PathPattern> const& patterns,
    std::string const& input) -> Result<std::vector<std::string>>
{
    auto result = std::vector<std::string> {};
    auto evaluator = parser::Evaluator { *ctx.eval };

    // Build pattern flags from input
    // For outputs, %d is the current directory basename (where the Tupfile is),
    // NOT the directory of the input file. This matches tup's behavior.
    auto current_dir_name = ctx.current_dir.empty()
        ? std::string { "." }
        : ctx.current_dir.filename().string();
    auto flags = parser::PatternFlags {
        .input = input,
        .input_base = std::string { parser::path_basename(input) },
        .input_noext = std::string { parser::path_stem(input) },
        .input_ext = std::string { parser::path_extension(input) },
        .input_dir = current_dir_name,
    };

    for (auto const& pattern : patterns) {
        if (pattern.is_group)
            continue; // Groups are not valid in outputs
        if (pattern.is_output_exclusion)
            continue; // Exclusion patterns are markers, not actual outputs

        auto paths = Result<std::vector<std::string>> { evaluator.expand_path(pattern) };
        if (!paths)
            return pup::unexpected<Error>(paths.error());

        for (auto& path : *paths) {
            // Expand pattern flags (%B, %f, etc.)
            auto expanded = Result<std::string> { evaluator.expand_pattern(path, flags) };
            auto output_path = expanded ? *expanded : std::move(path);

            // Map to output directory
            output_path = map_to_variant(output_path, ctx.current_dir, ctx.options.variant_dir,
                ctx.options.source_root, ctx.options.output_root);

            result.push_back(std::move(output_path));
        }
    }

    return result;
}

auto GraphBuilder::expand_command(
    BuilderContext& ctx,
    parser::Expression const& cmd,
    std::vector<std::string> const& inputs,
    std::vector<std::string> const& outputs) -> Result<std::string>
{
    auto evaluator = parser::Evaluator { *ctx.eval };

    // First get literal text from expression
    auto literal = Result<std::string> { evaluator.expand(cmd) };
    if (!literal)
        return pup::unexpected<Error>(literal.error());

    // Now expand variables in the literal text (handles $(VAR) references)
    auto expanded = Result<std::string> { evaluator.expand(std::string_view { *literal }) };
    if (!expanded)
        return pup::unexpected<Error>(expanded.error());

    // Transform paths to be relative to source directory (where command runs)
    // Input/output paths are project-root-relative, but commands run from source_dir
    auto source_to_root = std::string {};
    if (!ctx.current_dir.empty()) {
        for (auto const& comp : ctx.current_dir) {
            auto s = comp.string();
            if (s != "." && s != "/" && !s.empty())
                source_to_root += "../";
        }
    }

    auto current_dir_str = ctx.current_dir.string();
    auto make_source_relative = [&](std::string const& path) -> std::string {
        if (path.empty())
            return path;
        // Absolute paths (from out-of-tree builds) stay absolute
        if (!path.empty() && path[0] == '/')
            return path;
        // Don't transform paths that already start with ../
        if (path.size() >= 2 && path[0] == '.' && path[1] == '.')
            return path;
        if (source_to_root.empty())
            return path;
        // Local paths: strip current_dir prefix instead of round-trip via root
        // e.g., "src/lib/add.c" -> "add.c" (not "../../src/lib/add.c")
        if (!current_dir_str.empty() && path.starts_with(current_dir_str + "/"))
            return path.substr(current_dir_str.size() + 1);
        if (!current_dir_str.empty() && path == current_dir_str)
            return ".";
        // Cross-directory reference: use full relative path from source dir
        return source_to_root + path;
    };

    auto cmd_inputs = std::vector<std::string> {};
    cmd_inputs.reserve(inputs.size());
    for (auto const& inp : inputs)
        cmd_inputs.push_back(make_source_relative(inp));

    auto cmd_outputs = std::vector<std::string> {};
    cmd_outputs.reserve(outputs.size());
    for (auto const& out : outputs)
        cmd_outputs.push_back(make_source_relative(out));

    // Build pattern flags
    auto primary_input = cmd_inputs.empty() ? std::string {} : cmd_inputs[0];
    auto primary_output = cmd_outputs.empty() ? std::string {} : cmd_outputs[0];

    // %d is the current directory basename (where the Tupfile is),
    // NOT the directory of the input file. This matches tup's behavior.
    auto current_dir_name = ctx.current_dir.empty()
        ? std::string { "." }
        : ctx.current_dir.filename().string();

    auto flags = parser::PatternFlags {
        .input = primary_input,
        .input_base = std::string { parser::path_basename(primary_input) },
        .input_noext = std::string { parser::path_stem(primary_input) },
        .input_ext = std::string { parser::path_extension(primary_input) },
        .output = primary_output,
        .output_base = std::string { parser::path_basename(primary_output) },
        .input_dir = current_dir_name,
        .all_inputs = cmd_inputs,
    };

    // Expand pattern flags and return
    return evaluator.expand_pattern(*expanded, flags);
}

namespace {
constexpr auto MAX_DIRECTORY_DEPTH = 128;
}

auto GraphBuilder::get_or_create_directory_node(
    BuilderContext& ctx,
    std::filesystem::path const& dir_path,
    int depth) -> Result<NodeId>
{
    // Normalize first to handle ., .., and redundant separators
    auto normalized_path = dir_path.lexically_normal();

    // Root directory (empty, ".", or "/") has no parent - return 0
    if (normalized_path.empty() || normalized_path == "." || normalized_path == "/")
        return NodeId { 0 };

    // Guard against pathological recursion
    if (depth > MAX_DIRECTORY_DEPTH)
        return make_error<NodeId>(ErrorCode::InvalidArgument, "Directory nesting exceeds maximum depth");

    auto normalized = normalized_path.string();
    auto parent_path = normalized_path.parent_path();
    auto basename = normalized_path.filename().string();

    // Recurse to get/create parent directory
    auto parent_id_result = get_or_create_directory_node(ctx, parent_path, depth + 1);
    if (!parent_id_result)
        return parent_id_result;
    auto parent_id = *parent_id_result;

    // Check if directory already exists
    if (auto existing = ctx.graph->find_by_dir_name(parent_id, basename))
        return *existing;

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
    NodeType type) -> Result<NodeId>
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
    if (!parent_id_result)
        return parent_id_result;
    auto parent_id = *parent_id_result;

    // Check if node already exists
    if (auto existing = ctx.graph->find_by_dir_name(parent_id, basename))
        return *existing;

    // Create new node
    auto node = Node {
        .type = type,
        .name = basename,
        .parent_dir = parent_id,
    };

    return ctx.graph->add_node(std::move(node));
}

auto GraphBuilder::create_command_node(
    BuilderContext& ctx,
    std::string const& command,
    std::string const& display) -> Result<NodeId>
{
    auto node = Node {
        .type = NodeType::Command,
        .command = command,
        .display = display,
        .source_dir = ctx.current_dir.string(),
        .exported_vars = ctx.exported_vars,
    };

    auto cmd_id_result = ctx.graph->add_node(std::move(node));
    if (!cmd_id_result)
        return cmd_id_result;

    auto cmd_id = *cmd_id_result;

    // Add sticky edges from Tupfile and included files to this command
    for (auto src_id : ctx.sticky_sources)
        (void)ctx.graph->add_edge(src_id, cmd_id, LinkType::Sticky);

    return cmd_id;
}

} // namespace pup::graph
