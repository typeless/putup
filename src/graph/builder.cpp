// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/graph/builder.hpp"
#include "pup/parser/glob.hpp"

#include <algorithm>

namespace pup::graph {

GraphBuilder::GraphBuilder(BuilderOptions options)
    : options_(std::move(options))
{
}

auto GraphBuilder::build(parser::Tupfile const& tupfile, parser::EvalContext& eval)
    -> Result<BuildGraph>
{
    auto graph = BuildGraph {};
    auto result = Result<void>{add_tupfile(graph, tupfile, eval)};
    if (!result)
        return pup::unexpected<Error>(result.error());
    return graph;
}

auto GraphBuilder::add_tupfile(
    BuildGraph& graph,
    parser::Tupfile const& tupfile,
    parser::EvalContext& eval) -> Result<void>
{
    auto ctx = BuilderContext {
        .graph = &graph,
        .eval = &eval,
        .options = options_,
        .current_dir = std::filesystem::path { tupfile.filename }.parent_path(),
        .current_file = tupfile.filename,
    };

    for (auto const& stmt : tupfile.statements) {
        auto result = Result<void>{process_statement(ctx, *stmt)};
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

    // Other directives (include, export, etc.) are handled during parsing
    return {};
}

auto GraphBuilder::process_rule(
    BuilderContext& ctx,
    parser::Rule const& rule) -> Result<void>
{
    // Expand input patterns
    auto inputs = Result<std::vector<std::string>>{expand_inputs(ctx, rule.inputs)};
    if (!inputs)
        return pup::unexpected<Error>(inputs.error());

    if (rule.foreach_) {
        // Foreach rule: create one command per input
        for (auto const& input : *inputs) {
            auto result = Result<void>{expand_rule(ctx, rule, { input })};
            if (!result)
                return pup::unexpected<Error>(result.error());
        }
    } else {
        // Normal rule: single command for all inputs
        auto result = Result<void>{expand_rule(ctx, rule, *inputs)};
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
    };

    return {};
}

auto GraphBuilder::process_assignment(
    BuilderContext& ctx,
    parser::Assignment const& assign) -> Result<void>
{
    auto evaluator = parser::Evaluator { *ctx.eval };
    auto value = Result<std::string>{evaluator.expand(assign.value)};
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
        auto result = Result<void>{process_statement(ctx, *stmt)};
        if (!result)
            return pup::unexpected<Error>(result.error());
    }

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
    auto expanded_cmd = Result<std::string>{expand_command(ctx, rule.command, inputs, {})};
    if (!expanded_cmd)
        return pup::unexpected<Error>(expanded_cmd.error());

    auto cmd_str = std::string{*expanded_cmd};
    // Trim whitespace
    while (!cmd_str.empty() && (cmd_str.front() == ' ' || cmd_str.front() == '\t'))
        cmd_str.erase(0, 1);

    if (!cmd_str.empty() && cmd_str[0] == '!') {
        // Bang macro reference - look up the macro
        macro_name = cmd_str.substr(1);
        // Trim trailing whitespace from macro name
        while (!macro_name.empty() && (macro_name.back() == ' ' || macro_name.back() == '\t'))
            macro_name.pop_back();

        auto it = decltype(ctx.macros)::iterator{ctx.macros.find(macro_name)};
        if (it == ctx.macros.end())
            return make_error<void>(ErrorCode::UnknownMacro,
                "Unknown bang macro: !" + macro_name);

        macro_ptr = &it->second;

        // Use macro's outputs if rule doesn't specify any
        if (outputs_patterns.empty())
            outputs_patterns = macro_ptr->outputs;
    }

    // Expand outputs
    auto outputs = Result<std::vector<std::string>>{expand_outputs(ctx, outputs_patterns, primary_input)};
    if (!outputs)
        return pup::unexpected<Error>(outputs.error());

    // Now expand command with actual outputs for %o substitution
    if (macro_ptr) {
        auto macro_cmd = Result<std::string>{expand_command(ctx, macro_ptr->command, inputs, *outputs)};
        if (!macro_cmd)
            return pup::unexpected<Error>(macro_cmd.error());
        cmd_text = *macro_cmd;

        if (macro_ptr->display) {
            auto disp_result = Result<std::string>{expand_command(ctx, *macro_ptr->display, inputs, *outputs)};
            if (disp_result)
                display = *disp_result;
        }
    } else {
        auto full_cmd = Result<std::string>{expand_command(ctx, rule.command, inputs, *outputs)};
        if (!full_cmd)
            return pup::unexpected<Error>(full_cmd.error());
        cmd_text = *full_cmd;

        if (rule.display) {
            auto disp_result = Result<std::string>{expand_command(ctx, *rule.display, inputs, *outputs)};
            if (disp_result)
                display = *disp_result;
        }
    }

    // Create command node
    auto cmd_id = Result<NodeId>{create_command_node(ctx, cmd_text, display)};
    if (!cmd_id)
        return pup::unexpected<Error>(cmd_id.error());

    // Create edges from inputs to command
    for (auto const& input : inputs) {
        auto input_id = Result<NodeId>{get_or_create_file_node(ctx, input, NodeType::File)};
        if (!input_id)
            return pup::unexpected<Error>(input_id.error());
        auto edge_result = Result<void>{ctx.graph->add_edge(*input_id, *cmd_id)};
        if (!edge_result)
            return pup::unexpected<Error>(edge_result.error());
    }

    // Create edges from command to outputs
    for (auto const& output : *outputs) {
        auto output_id = Result<NodeId>{get_or_create_file_node(ctx, output, NodeType::Generated)};
        if (!output_id)
            return pup::unexpected<Error>(output_id.error());
        auto edge_result = Result<void>{ctx.graph->add_edge(*cmd_id, *output_id)};
        if (!edge_result)
            return pup::unexpected<Error>(edge_result.error());

        // Add to output group if specified
        if (rule.output_group)
            ctx.groups[*rule.output_group].push_back(*output_id);
    }

    // Handle order-only inputs
    for (auto const& pattern : rule.order_only_inputs) {
        auto order_inputs = Result<std::vector<std::string>>{expand_inputs(ctx, { pattern })};
        if (!order_inputs)
            continue;

        for (auto const& oi : *order_inputs) {
            auto oi_id = Result<NodeId>{get_or_create_file_node(ctx, oi, NodeType::File)};
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
    auto result = std::vector<std::string> {};
    auto evaluator = parser::Evaluator { *ctx.eval };

    for (auto const& pattern : patterns) {
        if (pattern.is_exclusion)
            continue; // Handle exclusions later

        if (pattern.is_group) {
            // Group reference
            auto it = decltype(ctx.groups)::iterator{ctx.groups.find(pattern.group_name)};
            if (it != ctx.groups.end()) {
                for (auto id : it->second) {
                    if (auto const* node = ctx.graph->get_node(id))
                        result.push_back(node->path);
                }
            }
            continue;
        }

        // Expand path expression
        auto paths = Result<std::vector<std::string>>{evaluator.expand_path(pattern)};
        if (!paths)
            return pup::unexpected<Error>(paths.error());

        for (auto& path : *paths) {
            // Expand globs if enabled
            if (ctx.options.expand_globs && parser::has_glob_chars(path)) {
                auto base = std::filesystem::path{ctx.current_dir.empty() ? ctx.options.root_dir
                                                    : ctx.options.root_dir / ctx.current_dir};

                // First try expanding against filesystem
                auto expanded = Result<std::vector<std::string>>{parser::glob_expand(path, base)};
                if (expanded && !expanded->empty()) {
                    for (auto& p : *expanded)
                        result.push_back(std::move(p));
                } else {
                    // No files on disk - look for matching Generated nodes in graph
                    auto glob = parser::Glob { path };
                    for (auto id : ctx.graph->nodes_of_type(NodeType::Generated)) {
                        if (auto const* node = ctx.graph->get_node(id)) {
                            if (glob.matches(node->path))
                                result.push_back(node->path);
                        }
                    }
                }
            } else {
                result.push_back(std::move(path));
            }
        }
    }

    // Handle exclusions
    for (auto const& pattern : patterns) {
        if (!pattern.is_exclusion)
            continue;

        auto paths = Result<std::vector<std::string>>{evaluator.expand_path(pattern)};
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

        auto paths = Result<std::vector<std::string>>{evaluator.expand_path(pattern)};
        if (!paths)
            return pup::unexpected<Error>(paths.error());

        for (auto& path : *paths) {
            // Expand pattern flags (%B, %f, etc.)
            auto expanded = Result<std::string>{evaluator.expand_pattern(path, flags)};
            if (expanded)
                result.push_back(*expanded);
            else
                result.push_back(std::move(path));
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
    auto literal = Result<std::string>{evaluator.expand(cmd)};
    if (!literal)
        return pup::unexpected<Error>(literal.error());

    // Now expand variables in the literal text (handles $(VAR) references)
    auto expanded = Result<std::string>{evaluator.expand(std::string_view { *literal })};
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
    if (auto existing = std::optional<NodeId>{ctx.graph->find_by_path(path)})
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
    };

    return ctx.graph->add_node(std::move(node));
}

} // namespace pup::graph
