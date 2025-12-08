// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/graph/builder.hpp"
#include "pup/parser/glob.hpp"
#include "pup/parser/parser.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace pup::graph {

namespace {

/// Map an output path to the variant directory (or current_dir if no variant)
/// e.g., "foo.o" in "src/lib" with variant "build" -> "build/src/lib/foo.o"
/// e.g., "foo.o" in "src/lib" with no variant -> "src/lib/foo.o"
auto map_to_variant(
    std::string const& path,
    std::filesystem::path const& current_dir,
    std::filesystem::path const& variant_dir) -> std::string
{
    if (variant_dir.empty()) {
        // No variant - still prefix with current_dir for proper project-relative path
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
    // Compute current_dir relative to root_dir
    auto tupfile_parent = std::filesystem::path { tupfile.filename }.parent_path();
    auto relative_dir = std::filesystem::relative(tupfile_parent, options_.root_dir);
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

    // Set up resolve_group callback for {group} pattern expansion
    eval.resolve_group = [&ctx](std::string_view name) -> std::vector<std::string> {
        auto it = ctx.groups.find(std::string { name });
        if (it == ctx.groups.end())
            return {};
        auto paths = std::vector<std::string> {};
        for (auto id : it->second) {
            if (auto const* node = ctx.graph->get_node(id))
                paths.push_back(node->path);
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
            if (auto const* node = ctx.graph->get_node(id))
                paths.push_back(node->path);
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
    };

    return {};
}

auto GraphBuilder::process_assignment(
    BuilderContext& ctx,
    parser::Assignment const& assign) -> Result<void>
{
    auto evaluator = parser::Evaluator { *ctx.eval };
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
        db->set(assign.name, *value);
        break;
    case parser::Assignment::Op::Append:
        db->append(assign.name, *value);
        break;
    case parser::Assignment::Op::Define:
        // := means no further expansion
        db->set(assign.name, *value);
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
    namespace fs = std::filesystem;

    // Find the include file path
    auto include_path = std::string {};

    if (inc.is_rules) {
        // include_rules: search up directory tree for Tuprules.tup
        auto search_dir = fs::path { ctx.options.root_dir / ctx.current_dir };
        auto root = fs::path { ctx.options.root_dir };

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

        auto resolved = fs::path { ctx.options.root_dir / ctx.current_dir / *path_result };
        if (!fs::exists(resolved))
            return make_error<void>(ErrorCode::IncludeNotFound,
                "Include file not found: " + *path_result);
        include_path = resolved.string();
    }

    // Prevent infinite recursion
    if (ctx.included_files.contains(include_path))
        return {};
    ctx.included_files.insert(include_path);

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
        auto rel_path = fs::relative(include_dir, ctx.options.root_dir / ctx.current_dir);
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
        // Bang macro reference - look up the macro
        macro_name = cmd_str.substr(1);
        // Trim trailing whitespace from macro name
        while (!macro_name.empty() && (macro_name.back() == ' ' || macro_name.back() == '\t'))
            macro_name.pop_back();

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

    // Create command node
    auto cmd_id = Result<NodeId> { create_command_node(ctx, cmd_text, display) };
    if (!cmd_id)
        return pup::unexpected<Error>(cmd_id.error());

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
        auto output_oo_group = rule.output_order_only_group;
        if (!output_oo_group && macro_ptr && macro_ptr->output_order_only_group)
            output_oo_group = macro_ptr->output_order_only_group;
        if (output_oo_group) {
            auto dir = ctx.current_dir.empty() ? "." : ctx.current_dir.string();
            auto key = GroupKey { dir, *output_oo_group };
            order_only_groups_[key].push_back(*output_id);
        }
    }

    // Handle order-only inputs (merge rule + macro if applicable)
    auto all_order_only = rule.order_only_inputs;
    if (macro_ptr && !macro_ptr->order_only_inputs.empty()) {
        all_order_only.insert(all_order_only.end(),
            macro_ptr->order_only_inputs.begin(),
            macro_ptr->order_only_inputs.end());
    }

    for (auto const& pattern : all_order_only) {
        auto order_inputs = Result<std::vector<std::string>> { expand_inputs(ctx, { pattern }) };
        if (!order_inputs)
            continue;

        for (auto const& oi : *order_inputs) {
            auto oi_id = Result<NodeId> { get_or_create_file_node(ctx, oi, NodeType::File) };
            if (oi_id)
                (void)ctx.graph->add_order_only_edge(*oi_id, *cmd_id);
        }
    }

    return {};
}

auto GraphBuilder::expand_inputs(
    BuilderContext& ctx,
    std::vector<parser::PathPattern> const& patterns) -> Result<std::vector<std::string>>
{
    namespace fs = std::filesystem;
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
                    if (auto const* node = ctx.graph->get_node(id))
                        result.push_back(node->path);
                }
            }
            continue;
        }

        if (pattern.is_order_only_group) {
            // Order-only group reference <name> - cross-directory
            // The pattern.path contains the directory prefix (e.g., $(ROOT)/include/generated/)
            // Expand it to get the actual directory
            auto group_dir = std::string {};
            if (!pattern.path.empty()) {
                auto expanded = Result<std::string> { evaluator.expand(pattern.path) };
                if (expanded) {
                    auto path = fs::path { *expanded }.lexically_normal();
                    // Make path relative to root if absolute within project
                    if (path.is_absolute())
                        path = fs::relative(path, ctx.options.root_dir);
                    // Combine with current_dir if relative
                    else if (!ctx.current_dir.empty())
                        path = (ctx.current_dir / path).lexically_normal();
                    group_dir = path.empty() ? "." : path.string();

                    // Demand-driven parsing: request the directory's Tupfile if not yet parsed
                    if (ctx.eval && ctx.eval->request_directory && ctx.eval->available_tupfile_dirs) {
                        if (ctx.eval->available_tupfile_dirs->contains(path)) {
                            auto req_result = Result<void> { ctx.eval->request_directory(path) };
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
                    if (auto const* node = ctx.graph->get_node(id))
                        result.push_back(node->path);
                }
            }
            continue;
        }

        // Expand path expression
        auto paths = Result<std::vector<std::string>> { evaluator.expand_path(pattern) };
        if (!paths)
            return pup::unexpected<Error>(paths.error());

        for (auto& path : *paths) {
            // Expand globs if enabled
            if (ctx.options.expand_globs && parser::has_glob_chars(path)) {
                auto base = std::filesystem::path { ctx.current_dir.empty() ? ctx.options.root_dir
                                                                            : ctx.options.root_dir / ctx.current_dir };

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
                    auto variant_path = map_to_variant(path, ctx.current_dir, ctx.options.variant_dir);
                    auto glob = parser::Glob { variant_path };
                    for (auto id : ctx.graph->nodes_of_type(NodeType::Generated)) {
                        if (auto const* node = ctx.graph->get_node(id)) {
                            if (glob.matches(node->path))
                                result.push_back(node->path);
                        }
                    }
                }
            } else {
                // Non-glob path: check if file exists on disk, or find in graph
                auto full_path = std::filesystem::path { ctx.options.root_dir / ctx.current_dir / path };
                if (std::filesystem::exists(full_path)) {
                    // Prefix with current_dir to make path relative to project root
                    if (!ctx.current_dir.empty())
                        result.push_back((ctx.current_dir / path).string());
                    else
                        result.push_back(std::move(path));
                } else {
                    // Not on disk - try demand-driven parsing of the file's directory
                    auto file_dir = fs::path { path }.parent_path();
                    auto abs_file_dir = fs::path { (ctx.current_dir / file_dir).lexically_normal() };
                    if (ctx.eval && ctx.eval->request_directory && ctx.eval->available_tupfile_dirs) {
                        if (ctx.eval->available_tupfile_dirs->contains(abs_file_dir)) {
                            auto req_result = Result<void> { ctx.eval->request_directory(abs_file_dir) };
                            if (!req_result)
                                return pup::unexpected<Error>(req_result.error());
                        }
                    }

                    // Check for generated file in variant
                    auto variant_path = map_to_variant(path, ctx.current_dir, ctx.options.variant_dir);
                    if (ctx.graph->find_by_path(variant_path)) {
                        result.push_back(variant_path);
                    } else {
                        // Fall back to original path
                        result.push_back(std::move(path));
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
            result.erase(
                std::remove(result.begin(), result.end(), excl),
                result.end());
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
    auto flags = parser::PatternFlags {
        .input = input,
        .input_base = std::string { parser::path_basename(input) },
        .input_noext = std::string { parser::path_stem(input) },
        .input_ext = std::string { parser::path_extension(input) },
        .input_dir = std::string { parser::path_directory(input) },
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

            // Map to variant directory if configured
            output_path = map_to_variant(output_path, ctx.current_dir, ctx.options.variant_dir);

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

    // Build pattern flags
    auto primary_input = inputs.empty() ? std::string {} : inputs[0];
    auto primary_output = outputs.empty() ? std::string {} : outputs[0];

    auto flags = parser::PatternFlags {
        .input = primary_input,
        .input_base = std::string { parser::path_basename(primary_input) },
        .input_noext = std::string { parser::path_stem(primary_input) },
        .input_ext = std::string { parser::path_extension(primary_input) },
        .output = primary_output,
        .output_base = std::string { parser::path_basename(primary_output) },
        .input_dir = std::string { parser::path_directory(primary_input) },
        .all_inputs = inputs,
    };

    // Expand pattern flags
    return evaluator.expand_pattern(*expanded, flags);
}

auto GraphBuilder::get_or_create_file_node(
    BuilderContext& ctx,
    std::string const& path,
    NodeType type) -> Result<NodeId>
{
    // Check if node already exists
    if (auto existing = std::optional<NodeId> { ctx.graph->find_by_path(path) })
        return *existing;

    // Create new node
    auto node = Node {
        .type = type,
        .path = path,
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
        .exported_vars = ctx.exported_vars,
    };

    return ctx.graph->add_node(std::move(node));
}

} // namespace pup::graph
