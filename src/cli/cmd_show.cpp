// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/multi_variant.hpp"
#include "pup/cli/output.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/node_id_map.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string_utils.hpp"
#include "pup/core/types.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/rule_pattern.hpp"
#include "pup/graph/topo.hpp"
#include "pup/index/entry.hpp"
#include "pup/index/reader.hpp"
#include "pup/parser/eval.hpp"
#include "pup/parser/var_tracking.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace pup::cli {

namespace {

auto load_index_for_all_deps(
    Options const& opts,
    ProjectLayout const& layout
) -> std::optional<pup::index::Index>
{
    if (!opts.include_all_deps) {
        return std::nullopt;
    }

    auto index_path = layout.index_path();
    if (!pup::platform::exists(index_path)) {
        fprintf(stderr, "Warning: No index found - run 'putup' first\n");
        return std::nullopt;
    }

    auto index_result = pup::index::read_index(index_path);
    if (!index_result) {
        return std::nullopt;
    }

    index_result->build_edge_indices();
    return std::move(*index_result);
}

auto format_node_id(pup::NodeId id) -> std::string
{
    char buf[32];
    if (node_id::is_command(id)) {
        snprintf(buf, sizeof(buf), "c%zu", node_id::index(id));
    } else {
        snprintf(buf, sizeof(buf), "f%zu", node_id::index(id));
    }
    return buf;
}

auto expand_script_run(std::string_view pattern, std::string_view dir, std::string_view cmd) -> std::string
{
    auto result = std::string {};
    result.reserve(pattern.size() + dir.size() + cmd.size());

    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '%' && i + 4 <= pattern.size()) {
            if (pattern.substr(i, 4) == "%DIR") {
                result += dir;
                i += 3;
                continue;
            }
            if (pattern.substr(i, 4) == "%CMD") {
                result += cmd;
                i += 3;
                continue;
            }
        }
        result += pattern[i];
    }
    return result;
}

auto cmd_export_script(Options const& opts, std::string_view variant_name) -> int
{
    auto scanner_registry = make_scanner_registry();
    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
        .require_config = true,
        .scanner_registry = scanner_registry ? &*scanner_registry : nullptr,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        fprintf(stderr, "[%.*s] Error: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), result.error().message.c_str());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;
    auto topo = pup::graph::topological_sort(ctx.graph());
    auto const& cfg = ctx.config_vars();

    auto script_prologue = cfg.get("SCRIPT_PROLOGUE");
    if (script_prologue.empty()) {
        fprintf(stderr, "[%.*s] Error: SCRIPT_PROLOGUE not set in config\n", static_cast<int>(variant_name.size()), variant_name.data());
        return EXIT_FAILURE;
    }

    auto script_run = cfg.get("SCRIPT_RUN");
    if (script_run.empty()) {
        fprintf(stderr, "[%.*s] Error: SCRIPT_RUN not set in config\n", static_cast<int>(variant_name.size()), variant_name.data());
        return EXIT_FAILURE;
    }

    auto script_mkdir = cfg.get("SCRIPT_MKDIR");
    if (script_mkdir.empty()) {
        fprintf(stderr, "[%.*s] Error: SCRIPT_MKDIR not set in config\n", static_cast<int>(variant_name.size()), variant_name.data());
        return EXIT_FAILURE;
    }

    auto script_comment = cfg.get("SCRIPT_COMMENT");
    if (script_comment.empty()) {
        fprintf(stderr, "[%.*s] Error: SCRIPT_COMMENT not set in config\n", static_cast<int>(variant_name.size()), variant_name.data());
        return EXIT_FAILURE;
    }

    printf("%.*s\n\n", static_cast<int>(script_prologue.size()), script_prologue.data());

    auto output_dirs = std::vector<std::string> {};
    for (auto id : ctx.graph().all_nodes()) {
        auto const* node = ctx.graph().get_file_node(id);
        if (!node) {
            continue;
        }
        if (node->type != pup::NodeType::Generated && node->type != pup::NodeType::File) {
            continue;
        }

        auto node_path = ctx.graph().get_full_path(id);
        if (node_path.empty()) {
            continue;
        }

        auto inputs = ctx.graph().get_inputs(id);
        for (auto input_id : inputs) {
            if (node_id::is_command(input_id)) {
                auto path = std::string { node_path };
                if (!pup::path::parent(path).empty()) {
                    auto parent = std::string { pup::path::parent(path) };
                    if (!parent.empty() && parent != ".") {
                        output_dirs.push_back(std::move(parent));
                    }
                }
                break;
            }
        }
    }

    std::ranges::sort(output_dirs);
    output_dirs.erase(std::unique(output_dirs.begin(), output_dirs.end()), output_dirs.end());

    if (!output_dirs.empty()) {
        printf("%.*s Create output directories\n", static_cast<int>(script_comment.size()), script_comment.data());
        for (auto const& dir : output_dirs) {
            auto line = expand_script_run(script_mkdir, dir, "");
            printf("%s\n", line.c_str());
        }
        printf("\n");
    }

    for (auto id : topo.order) {
        if (!node_id::is_command(id)) {
            continue;
        }
        auto const* node = ctx.graph().get_command_node(id);
        if (!node) {
            continue;
        }

        if (node->output_action == graph::OutputAction::InjectImplicitDeps) {
            continue;
        }

        auto source_dir = graph::get_source_dir(ctx.graph().graph(), id);
        auto dir = source_dir.empty() ? std::string { "." } : std::string { source_dir };
        auto cmd = std::string { graph::expand_instruction(ctx.graph().graph(), id) };

        auto line = expand_script_run(script_run, dir, cmd);
        printf("%s\n", line.c_str());
    }

    auto script_epilogue = cfg.get("SCRIPT_EPILOGUE");
    if (!script_epilogue.empty()) {
        printf("\n%.*s\n", static_cast<int>(script_epilogue.size()), script_epilogue.data());
    }

    return EXIT_SUCCESS;
}

auto cmd_export_graph(Options const& opts, std::string_view variant_name) -> int
{
    auto scanner_registry = make_scanner_registry();
    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
        .require_config = true,
        .scanner_registry = scanner_registry ? &*scanner_registry : nullptr,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        fprintf(stderr, "[%.*s] Error: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), result.error().message.c_str());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;
    auto index = load_index_for_all_deps(opts, ctx.layout());

    if (opts.summary) {
        auto commands = ctx.graph().nodes_of_type(pup::NodeType::Command);
        printf("[%.*s] Tupfiles: %zu\n", static_cast<int>(variant_name.size()), variant_name.data(), ctx.parsed_dirs().size());
        printf("[%.*s] Nodes: %zu\n", static_cast<int>(variant_name.size()), variant_name.data(), ctx.graph().node_count());
        printf("[%.*s] Edges: %zu\n", static_cast<int>(variant_name.size()), variant_name.data(), ctx.graph().edge_count());
        printf("[%.*s] Commands: %zu\n", static_cast<int>(variant_name.size()), variant_name.data(), commands.size());

        if (index) {
            auto implicit_count = std::size_t { 0 };
            for (auto const& edge : index->edges()) {
                if (edge.type == pup::LinkType::Implicit) {
                    ++implicit_count;
                }
            }
            printf("[%.*s] Implicit edges: %zu\n", static_cast<int>(variant_name.size()), variant_name.data(), implicit_count);
        }

        if (opts.verbose) {
            printf("[%.*s] Commands:\n", static_cast<int>(variant_name.size()), variant_name.data());
            for (auto id : commands) {
                if (ctx.graph().get_command_node(id)) {
                    auto display_sv = graph::get_display_str(ctx.graph().graph(), id);
                    auto cmd_sv = graph::expand_instruction(ctx.graph().graph(), id);
                    auto display = std::string { display_sv.empty() ? cmd_sv : display_sv };
                    printf("[%.*s]   %s\n", static_cast<int>(variant_name.size()), variant_name.data(), display.c_str());
                }
            }
        }
        return EXIT_SUCCESS;
    }

    printf("digraph G {\n");
    printf("  rankdir=LR;\n");

    auto declared_nodes = pup::NodeIdMap32 {};
    for (auto id : ctx.graph().all_nodes()) {
        declared_nodes.set(id, 1);

        auto get_label = [&]() -> std::string {
            if (node_id::is_command(id)) {
                auto const* cmd = ctx.graph().get_command_node(id);
                if (!cmd) {
                    return "";
                }
                auto display_sv = graph::get_display_str(ctx.graph().graph(), id);
                auto cmd_sv = graph::expand_instruction(ctx.graph().graph(), id);
                return std::string { display_sv.empty() ? cmd_sv : display_sv };
            }
            return ctx.graph().get_full_path(id);
        };
        auto label = escape_dot_label(get_label());

        printf("  %s [label=\"%s\"];\n", format_node_id(id).c_str(), label.c_str());

        for (auto input_id : ctx.graph().get_inputs(id)) {
            printf("  %s -> %s;\n", format_node_id(input_id).c_str(), format_node_id(id).c_str());
        }

        // Output order-only edges (dotted)
        for (auto oo_id : ctx.graph().get_order_only(id)) {
            printf("  %s -> %s [style=dotted color=\"#0088ff\"];\n", format_node_id(oo_id).c_str(), format_node_id(id).c_str());
        }
    }

    if (index) {
        auto implicit_nodes = pup::NodeIdMap32 {};

        for (auto const& edge : index->edges()) {
            if (edge.type != pup::LinkType::Implicit) {
                continue;
            }

            auto from_id = edge.from;
            auto to_id = edge.to;

            if (!declared_nodes.contains(to_id)) {
                continue;
            }

            if (!declared_nodes.contains(from_id)) {
                if (!implicit_nodes.contains(from_id)) {
                    implicit_nodes.set(from_id, 1);
                    auto const* file = index->find_file_by_id(from_id);
                    auto label = file ? escape_dot_label(file->path) : format_node_id(from_id);
                    printf("  %s [label=\"%s\" style=filled fillcolor=\"#f0f0f0\"];\n", format_node_id(from_id).c_str(), label.c_str());
                }
            }

            printf("  %s -> %s [style=dashed color=\"#888888\"];\n", format_node_id(from_id).c_str(), format_node_id(to_id).c_str());
        }
    }

    printf("}\n");
    return EXIT_SUCCESS;
}

auto cmd_export_compdb(Options const& opts, std::string_view variant_name) -> int
{
    auto scanner_registry = make_scanner_registry();
    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
        .require_config = true,
        .scanner_registry = scanner_registry ? &*scanner_registry : nullptr,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        fprintf(stderr, "[%.*s] Error: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), result.error().message.c_str());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;

    printf("[\n");
    auto commands = ctx.graph().nodes_of_type(pup::NodeType::Command);
    auto first = true;

    for (auto id : commands) {
        auto const* node = ctx.graph().get_command_node(id);
        if (!node) {
            continue;
        }

        auto source_file = std::string {};
        for (auto input_id : ctx.graph().get_inputs(id)) {
            auto input_path = ctx.graph().get_full_path(input_id);
            if (input_path.empty()) {
                continue;
            }
            if (input_path.ends_with(".c") || input_path.ends_with(".cc") || input_path.ends_with(".cpp") || input_path.ends_with(".cxx") || input_path.ends_with(".C") || input_path.ends_with(".S") || input_path.ends_with(".s")) {
                source_file = std::move(input_path);
                break;
            }
        }

        auto output_file = std::string {};
        for (auto output_id : ctx.graph().get_outputs(id)) {
            auto output_path = ctx.graph().get_full_path(output_id);
            if (output_path.empty()) {
                continue;
            }
            if (output_path.ends_with(".o") || output_path.ends_with(".obj")) {
                output_file = std::move(output_path);
                break;
            }
        }

        if (source_file.empty()) {
            continue;
        }

        auto source_dir_sv = graph::get_source_dir(ctx.graph().graph(), id);
        auto working_dir = ctx.layout().source_root;
        if (!source_dir_sv.empty()) {
            working_dir = pup::path::join(working_dir, std::string { source_dir_sv });
        }

        // Convert project-root-relative paths to working-dir-relative
        auto source_abs = pup::path::join(ctx.layout().source_root, source_file);
        auto source_rel = pup::path::relative(source_abs, working_dir);

        auto output_rel = std::string {};
        if (!output_file.empty()) {
            // Generated files exist at output_root
            auto output_abs = pup::path::join(ctx.layout().output_root, output_file);
            output_rel = pup::path::relative(output_abs, working_dir);
        }

        auto cmd_sv = graph::expand_instruction(ctx.graph().graph(), id);
        auto args = pup::core::tokenize_shell_command(cmd_sv);
        if (args.empty()) {
            continue;
        }

        if (!first) {
            printf(",\n");
        }
        first = false;

        printf("  {\n");
        printf("    \"directory\": \"%s\",\n", escape_json(working_dir).c_str());

        printf("    \"arguments\": [");
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i > 0) {
                printf(", ");
            }
            printf("\"%s\"", escape_json(args[i]).c_str());
        }
        printf("],\n");

        printf("    \"file\": \"%s\"", escape_json(source_rel).c_str());
        if (!output_rel.empty()) {
            printf(",\n    \"output\": \"%s\"", escape_json(output_rel).c_str());
        }
        printf("\n  }");
    }

    printf("\n]\n");
    return EXIT_SUCCESS;
}

auto output_var_text(
    std::vector<parser::VarHistory> const& histories
) -> int
{
    for (auto const& history : histories) {
        printf("%s = %s\n", history.name.c_str(), history.final_value.c_str());
        printf("  History:\n");
        for (auto const* assign : history.assignments) {
            auto const* prefix = assign->is_effective ? "  " : "# ";
            auto op_str = parser::op_to_string(assign->op);
            printf("  %s%s:%u\t%s %.*s %s", prefix, assign->filename.c_str(), assign->line, assign->name.c_str(), static_cast<int>(op_str.size()), op_str.data(), assign->value_after.c_str());
            if (!assign->is_effective) {
                printf("   (ineffective)");
            }
            printf("\n");
        }
        printf("\n");
    }
    return EXIT_SUCCESS;
}

auto output_var_json(
    std::vector<parser::VarHistory> const& histories,
    std::string_view variant_name
) -> int
{
    printf("{\n");
    printf("  \"variant\": \"%.*s\",\n", static_cast<int>(variant_name.size()), variant_name.data());
    printf("  \"variables\": {\n");

    auto first_var = true;
    for (auto const& history : histories) {
        if (!first_var) {
            printf(",\n");
        }
        first_var = false;

        printf("    \"%s\": {\n", escape_json(history.name).c_str());
        printf("      \"value\": \"%s\",\n", escape_json(history.final_value).c_str());
        printf("      \"history\": [\n");

        auto first_assign = true;
        for (auto const* assign : history.assignments) {
            if (!first_assign) {
                printf(",\n");
            }
            first_assign = false;

            auto op_str = parser::op_to_string(assign->op);
            printf("        {\n");
            printf("          \"file\": \"%s\",\n", escape_json(assign->filename).c_str());
            printf("          \"line\": %u,\n", assign->line);
            printf("          \"op\": \"%.*s\",\n", static_cast<int>(op_str.size()), op_str.data());
            printf("          \"value\": \"%s\",\n", escape_json(assign->value_after).c_str());
            printf("          \"effective\": %s\n", assign->is_effective ? "true" : "false");
            printf("        }");
        }

        printf("\n      ]\n");
        printf("    }");
    }

    printf("\n  }\n");
    printf("}\n");
    return EXIT_SUCCESS;
}

auto cmd_export_var(Options const& opts, std::string_view variant_name) -> int
{
    auto log = parser::AssignmentLog {};

    auto on_var_assigned = [&log](
                               std::string_view name,
                               parser::Assignment::Op op,
                               std::string_view value_before,
                               std::string_view value_after,
                               std::string_view filename,
                               std::uint32_t line,
                               std::uint32_t column,
                               bool is_effective
                           ) {
        log.push_back(parser::VarAssignment {
            .name = std::string { name },
            .filename = std::string { filename },
            .line = line,
            .column = column,
            .op = op,
            .value_before = std::string { value_before },
            .value_after = std::string { value_after },
            .is_effective = is_effective,
        });
    };

    auto scanner_registry = make_scanner_registry();
    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
        .require_config = true,
        .scanner_registry = scanner_registry ? &*scanner_registry : nullptr,
        .on_var_assigned = on_var_assigned,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        fprintf(stderr, "[%.*s] Error: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), result.error().message.c_str());
        return EXIT_FAILURE;
    }

    auto filtered = opts.show_var_filter.empty()
        ? log
        : parser::filter_by_name(log, opts.show_var_filter);

    auto histories = parser::group_by_name(filtered);

    return opts.show_json
        ? output_var_json(histories, variant_name)
        : output_var_text(histories);
}

auto cmd_export_instructions(Options const& opts, std::string_view variant_name) -> int
{
    auto scanner_registry = make_scanner_registry();
    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
        .require_config = true,
        .scanner_registry = scanner_registry ? &*scanner_registry : nullptr,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        fprintf(stderr, "[%.*s] Error: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), result.error().message.c_str());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;
    auto const& graph = ctx.graph().graph();

    // Build instruction usage map: instruction_id -> list of command IDs using it
    auto instruction_usage = std::vector<std::pair<StringId, std::vector<NodeId>>> {};

    for (auto const& cmd : graph.commands) {
        if (!is_empty(cmd.instruction_id)) {
            auto pos = std::lower_bound(instruction_usage.begin(), instruction_usage.end(), cmd.instruction_id, [](auto const& p, auto const& k) { return p.first < k; });
            if (pos != instruction_usage.end() && pos->first == cmd.instruction_id) {
                pos->second.push_back(cmd.id);
            } else {
                instruction_usage.emplace(pos, cmd.instruction_id, std::vector<NodeId> { cmd.id });
            }
        }
    }

    auto total_commands = graph.commands.size();
    auto unique_instructions = instruction_usage.size();

    printf("Instruction Analysis:\n");
    printf("  Commands: %zu\n", total_commands);
    printf("  Unique instructions: %zu\n", unique_instructions);
    if (unique_instructions > 0) {
        printf("  Deduplication ratio: %.1fx\n", double(total_commands) / double(unique_instructions));
    }

    // Sort instructions by usage count (descending)
    auto sorted_instructions = std::vector<std::pair<StringId, std::size_t>> {};
    sorted_instructions.reserve(instruction_usage.size());
    for (auto const& [iid, cmds] : instruction_usage) {
        sorted_instructions.emplace_back(iid, cmds.size());
    }
    std::sort(sorted_instructions.begin(), sorted_instructions.end(), [](auto const& a, auto const& b) {
        return a.second > b.second;
    });

    // Show top 10 instructions
    printf("\nTop instructions:\n");
    auto shown = std::size_t { 0 };
    for (auto const& [iid, count] : sorted_instructions) {
        if (shown >= 10) {
            break;
        }
        auto instruction_str = graph.strings.get(iid);
        // Truncate long instructions for display
        auto display_str = std::string { instruction_str };
        if (display_str.size() > 60) {
            display_str = display_str.substr(0, 57) + "...";
        }
        printf("  #%zu (%zu uses): \"%s\"\n", shown + 1, count, display_str.c_str());
        ++shown;
    }

    auto unique_instruction_bytes = std::size_t { 0 };
    for (auto const& [iid, _] : instruction_usage) {
        unique_instruction_bytes += graph.strings.get(iid).size();
    }

    auto per_command_overhead = total_commands * 8; // instruction_id + operand offset
    printf("\nStorage:\n");
    printf("  Unique instruction strings: %zu bytes\n", unique_instruction_bytes);
    printf("  Per-command overhead: %zu bytes (%zu commands x 8)\n", per_command_overhead, total_commands);
    printf("  Total: %zu bytes\n", unique_instruction_bytes + per_command_overhead);

    return EXIT_SUCCESS;
}

auto show_single_variant(Options const& opts, std::string_view variant_name) -> int
{
    if (opts.show_format == "script") {
        return cmd_export_script(opts, variant_name);
    }
    if (opts.show_format == "compdb") {
        return cmd_export_compdb(opts, variant_name);
    }
    if (opts.show_format == "graph") {
        return cmd_export_graph(opts, variant_name);
    }
    if (opts.show_format == "var") {
        return cmd_export_var(opts, variant_name);
    }
    if (opts.show_format == "instructions" || opts.show_format == "templates") {
        return cmd_export_instructions(opts, variant_name);
    }

    fprintf(stderr, "Unknown show format: %.*s\n", static_cast<int>(opts.show_format.size()), opts.show_format.data());
    fprintf(stderr, "Formats: script, compdb, graph, var, instructions\n");
    return EXIT_FAILURE;
}

} // namespace

auto cmd_show(Options const& opts) -> int
{
    if (opts.show_format.empty()) {
        fprintf(stderr, "Usage: putup show <format>\n");
        fprintf(stderr, "Formats: script, compdb, graph, var, instructions\n");
        return EXIT_FAILURE;
    }

    return for_each_variant(opts, show_single_variant, "Showing");
}

} // namespace pup::cli
