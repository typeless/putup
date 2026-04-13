// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/multi_variant.hpp"
#include "pup/cli/options.hpp"
#include "pup/cli/output.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/node_id_map.hpp"
#include "pup/core/path.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_utils.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/topo.hpp"
#include "pup/index/entry.hpp"
#include "pup/index/reader.hpp"
#include "pup/parser/ast.hpp"
#include "pup/parser/eval.hpp"
#include "pup/parser/var_tracking.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>

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

    auto index_path_sv = pup::global_pool().get(layout.index_path());
    if (!pup::platform::exists(index_path_sv)) {
        fprintf(stderr, "Warning: No index found - run 'putup' first\n");
        return std::nullopt;
    }

    auto index_result = pup::index::read_index(index_path_sv);
    if (!index_result) {
        return std::nullopt;
    }

    index_result->build_edge_indices();
    return std::move(*index_result);
}

auto format_node_id(pup::NodeId id) -> std::string_view
{
    auto& pool = global_pool();
    auto b = Buf {};
    if (node_id::is_command(id)) {
        b.fmt("c{}", node_id::index(id));
    } else {
        b.fmt("f{}", node_id::index(id));
    }
    return pool.get(b.intern(pool));
}

auto expand_script_run(std::string_view pattern, std::string_view dir, std::string_view cmd) -> StringId
{
    auto result = Buf {};
    result.reserve(pattern.size() + dir.size() + cmd.size());

    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '%' && i + 4 <= pattern.size()) {
            if (pattern.substr(i, 4) == "%DIR") {
                result.append(dir);
                i += 3;
                continue;
            }
            if (pattern.substr(i, 4) == "%CMD") {
                result.append(cmd);
                i += 3;
                continue;
            }
        }
        result.append(pattern[i]);
    }
    return result.intern(global_pool());
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
        fprintf(stderr, "[%.*s] Error: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), result.error().msg().data());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;
    auto topo = pup::graph::topological_sort(ctx.graph().graph);
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

    auto output_dirs = Vec<StringId> {};
    for (auto id : graph::all_nodes(ctx.graph().graph)) {
        auto node_type = graph::get_node_type(ctx.graph().graph, id);
        if (node_type != pup::NodeType::Generated && node_type != pup::NodeType::File) {
            continue;
        }

        auto node_path_id = graph::get_full_path(ctx.graph().graph, id);
        auto node_path = global_pool().get(node_path_id);
        if (node_path.empty()) {
            continue;
        }

        auto inputs = graph::get_inputs(ctx.graph().graph, id);
        for (auto input_id : inputs) {
            if (node_id::is_command(input_id)) {
                auto parent = pup::path::parent(node_path);
                if (!parent.empty() && parent != ".") {
                    output_dirs.push_back(global_pool().intern(parent));
                }
                break;
            }
        }
    }

    std::sort(output_dirs.begin(), output_dirs.end());
    output_dirs.erase(std::unique(output_dirs.begin(), output_dirs.end()), output_dirs.end());

    if (!output_dirs.empty()) {
        printf("%.*s Create output directories\n", static_cast<int>(script_comment.size()), script_comment.data());
        for (auto dir_id : output_dirs) {
            auto line = expand_script_run(script_mkdir, global_pool().get(dir_id), "");
            auto line_sv = global_pool().get(line);
            printf("%.*s\n", static_cast<int>(line_sv.size()), line_sv.data());
        }
        printf("\n");
    }

    for (auto id : topo.order) {
        if (!node_id::is_command(id)) {
            continue;
        }
        auto const* node = graph::get_command_node(ctx.graph().graph, id);
        if (!node) {
            continue;
        }

        if (node->output_action == graph::OutputAction::InjectImplicitDeps) {
            continue;
        }

        auto source_dir = graph::get_source_dir(ctx.graph().graph, id);
        auto dir = source_dir.empty() ? std::string_view { "." } : source_dir;
        auto cmd_id = graph::expand_instruction(ctx.graph().graph, id);

        auto line = expand_script_run(script_run, dir, global_pool().get(cmd_id));
        auto line_sv = global_pool().get(line);
        printf("%.*s\n", static_cast<int>(line_sv.size()), line_sv.data());
    }

    auto script_epilogue = cfg.get("SCRIPT_EPILOGUE");
    if (!script_epilogue.empty()) {
        printf("\n%.*s\n", static_cast<int>(script_epilogue.size()), script_epilogue.data());
    }

    return EXIT_SUCCESS;
}

auto cmd_export_graph(Options const& opts, std::string_view variant_name) -> int
{
    auto& pool = global_pool();
    auto scanner_registry = make_scanner_registry();
    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
        .require_config = true,
        .scanner_registry = scanner_registry ? &*scanner_registry : nullptr,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        fprintf(stderr, "[%.*s] Error: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), result.error().msg().data());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;
    auto index = load_index_for_all_deps(opts, ctx.layout());

    if (opts.summary) {
        auto commands = graph::nodes_of_type(ctx.graph().graph, pup::NodeType::Command);
        printf("[%.*s] Tupfiles: %zu\n", static_cast<int>(variant_name.size()), variant_name.data(), ctx.parsed_dirs().size());
        printf("[%.*s] Nodes: %zu\n", static_cast<int>(variant_name.size()), variant_name.data(), graph::node_count(ctx.graph().graph));
        printf("[%.*s] Edges: %zu\n", static_cast<int>(variant_name.size()), variant_name.data(), graph::edge_count(ctx.graph().graph));
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
                auto display_sv = graph::get_display_str(ctx.graph().graph, id);
                auto cmd_str_id = graph::expand_instruction(ctx.graph().graph, id);
                auto display = display_sv.empty() ? global_pool().get(cmd_str_id) : display_sv;
                printf("[%.*s]   %.*s\n", static_cast<int>(variant_name.size()), variant_name.data(), static_cast<int>(display.size()), display.data());
            }
        }
        return EXIT_SUCCESS;
    }

    printf("digraph G {\n");
    printf("  rankdir=LR;\n");

    auto declared_nodes = pup::NodeIdMap32 {};
    for (auto id : graph::all_nodes(ctx.graph().graph)) {
        declared_nodes.set(id, 1);

        auto get_label = [&]() -> std::string_view {
            if (node_id::is_command(id)) {
                auto display_sv = graph::get_display_str(ctx.graph().graph, id);
                auto cmd_str_id = graph::expand_instruction(ctx.graph().graph, id);
                return display_sv.empty() ? pool.get(cmd_str_id) : display_sv;
            }
            return pool.get(graph::get_full_path(ctx.graph().graph, id));
        };
        auto label = escape_dot_label(get_label());

        printf("  %s [label=\"%s\"];\n", format_node_id(id).data(), pool.get(label).data());

        for (auto input_id : graph::get_inputs(ctx.graph().graph, id)) {
            printf("  %s -> %s;\n", format_node_id(input_id).data(), format_node_id(id).data());
        }

        // Output order-only edges (dotted)
        for (auto oo_id : graph::get_order_only(ctx.graph().graph, id)) {
            printf("  %s -> %s [style=dotted color=\"#0088ff\"];\n", format_node_id(oo_id).data(), format_node_id(id).data());
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
                    auto label_id = file ? escape_dot_label(pool.get(file->path))
                                         : pool.intern(format_node_id(from_id));
                    printf("  %s [label=\"%s\" style=filled fillcolor=\"#f0f0f0\"];\n", format_node_id(from_id).data(), pool.get(label_id).data());
                }
            }

            printf("  %s -> %s [style=dashed color=\"#888888\"];\n", format_node_id(from_id).data(), format_node_id(to_id).data());
        }
    }

    printf("}\n");
    return EXIT_SUCCESS;
}

auto cmd_export_compdb(Options const& opts, std::string_view variant_name) -> int
{
    auto& pool = global_pool();
    auto scanner_registry = make_scanner_registry();
    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
        .require_config = true,
        .scanner_registry = scanner_registry ? &*scanner_registry : nullptr,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        fprintf(stderr, "[%.*s] Error: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), result.error().msg().data());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;

    printf("[\n");
    auto commands = graph::nodes_of_type(ctx.graph().graph, pup::NodeType::Command);
    auto first = true;

    for (auto id : commands) {
        auto source_file = std::string_view {};
        for (auto input_id : graph::get_inputs(ctx.graph().graph, id)) {
            auto input_path_id = graph::get_full_path(ctx.graph().graph, input_id);
            auto sv = pool.get(input_path_id);
            if (sv.empty()) {
                continue;
            }
            if (sv.ends_with(".c") || sv.ends_with(".cc") || sv.ends_with(".cpp") || sv.ends_with(".cxx") || sv.ends_with(".C") || sv.ends_with(".S") || sv.ends_with(".s")) {
                source_file = sv;
                break;
            }
        }

        auto output_file = std::string_view {};
        for (auto output_id : graph::get_outputs(ctx.graph().graph, id)) {
            auto output_path_id = graph::get_full_path(ctx.graph().graph, output_id);
            auto sv = pool.get(output_path_id);
            if (sv.empty()) {
                continue;
            }
            if (sv.ends_with(".o") || sv.ends_with(".obj")) {
                output_file = sv;
                break;
            }
        }

        if (source_file.empty()) {
            continue;
        }

        auto& pool = global_pool();
        auto source_root_sv = pool.get(ctx.layout().source_root);
        auto output_root_sv = pool.get(ctx.layout().output_root);
        auto source_dir_sv = graph::get_source_dir(ctx.graph().graph, id);
        auto working_dir = source_dir_sv.empty() ? source_root_sv : pool.get(pup::path::join(source_root_sv, source_dir_sv));

        // Convert project-root-relative paths to working-dir-relative
        auto source_abs = pool.get(pup::path::join(source_root_sv, source_file));
        auto source_rel = pool.get(pup::path::relative(source_abs, working_dir));

        auto output_rel = std::string_view {};
        if (!output_file.empty()) {
            auto output_abs = pool.get(pup::path::join(output_root_sv, output_file));
            output_rel = pool.get(pup::path::relative(output_abs, working_dir));
        }

        auto cmd_str_id = graph::expand_instruction(ctx.graph().graph, id);
        auto args = pup::core::tokenize_shell_command(pool.get(cmd_str_id));
        if (args.empty()) {
            continue;
        }

        if (!first) {
            printf(",\n");
        }
        first = false;

        printf("  {\n");
        printf("    \"directory\": \"%s\",\n", pool.get(escape_json(working_dir)).data());

        printf("    \"arguments\": [");
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i > 0) {
                printf(", ");
            }
            printf("\"%s\"", pool.get(escape_json(pool.get(args[i]))).data());
        }
        printf("],\n");

        printf("    \"file\": \"%s\"", pool.get(escape_json(source_rel)).data());
        if (!output_rel.empty()) {
            printf(",\n    \"output\": \"%s\"", pool.get(escape_json(output_rel)).data());
        }
        printf("\n  }");
    }

    printf("\n]\n");
    return EXIT_SUCCESS;
}

auto output_var_text(
    Vec<parser::VarHistory> const& histories
) -> int
{
    for (auto const& history : histories) {
        printf("%s = %s\n", global_pool().get(history.name).data(), global_pool().get(history.final_value).data());
        printf("  History:\n");
        for (auto const* assign : history.assignments) {
            auto const* prefix = assign->is_effective ? "  " : "# ";
            auto op_str = parser::op_to_string(assign->op);
            printf("  %s%s:%u\t%s %.*s %s", prefix, global_pool().get(assign->filename).data(), assign->line, global_pool().get(assign->name).data(), static_cast<int>(op_str.size()), op_str.data(), global_pool().get(assign->value_after).data());
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
    Vec<parser::VarHistory> const& histories,
    std::string_view variant_name
) -> int
{
    auto& pool = global_pool();
    printf("{\n");
    printf("  \"variant\": \"%.*s\",\n", static_cast<int>(variant_name.size()), variant_name.data());
    printf("  \"variables\": {\n");

    auto first_var = true;
    for (auto const& history : histories) {
        if (!first_var) {
            printf(",\n");
        }
        first_var = false;

        printf("    \"%s\": {\n", pool.get(escape_json(global_pool().get(history.name))).data());
        printf("      \"value\": \"%s\",\n", pool.get(escape_json(global_pool().get(history.final_value))).data());
        printf("      \"history\": [\n");

        auto first_assign = true;
        for (auto const* assign : history.assignments) {
            if (!first_assign) {
                printf(",\n");
            }
            first_assign = false;

            auto op_str = parser::op_to_string(assign->op);
            printf("        {\n");
            printf("          \"file\": \"%s\",\n", pool.get(escape_json(global_pool().get(assign->filename))).data());
            printf("          \"line\": %u,\n", assign->line);
            printf("          \"op\": \"%.*s\",\n", static_cast<int>(op_str.size()), op_str.data());
            printf("          \"value\": \"%s\",\n", pool.get(escape_json(global_pool().get(assign->value_after))).data());
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
        auto& pool = global_pool();
        log.push_back(parser::VarAssignment {
            .name = pool.intern(name),
            .filename = pool.intern(filename),
            .line = line,
            .column = column,
            .op = op,
            .value_before = pool.intern(value_before),
            .value_after = pool.intern(value_after),
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
        fprintf(stderr, "[%.*s] Error: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), result.error().msg().data());
        return EXIT_FAILURE;
    }

    auto filtered = is_empty(opts.show_var_filter)
        ? log
        : parser::filter_by_name(log, global_pool().get(opts.show_var_filter));

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
        fprintf(stderr, "[%.*s] Error: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), result.error().msg().data());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;
    auto const& graph = ctx.graph().graph;

    // Build instruction usage map: instruction_id -> list of command IDs using it
    auto instruction_usage = Vec<std::pair<StringId, Vec<NodeId>>> {};

    for (auto const& cmd : graph.commands) {
        if (!is_empty(cmd.instruction_id)) {
            auto pos = std::lower_bound(instruction_usage.begin(), instruction_usage.end(), cmd.instruction_id, [](auto const& p, auto const& k) { return p.first < k; });
            if (pos != instruction_usage.end() && pos->first == cmd.instruction_id) {
                pos->second.push_back(cmd.id);
            } else {
                instruction_usage.insert(pos, std::pair<StringId, Vec<NodeId>> { cmd.instruction_id, { cmd.id } });
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
    auto sorted_instructions = Vec<std::pair<StringId, std::size_t>> {};
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
        auto instruction_str = pup::global_pool().get(iid);
        // Truncate long instructions for display
        auto display_sv = instruction_str.size() > 60 ? instruction_str.substr(0, 57) : instruction_str;
        if (instruction_str.size() > 60) {
            auto tb = Buf {};
            tb.append(display_sv);
            tb.append("...");
            printf("  #%zu (%zu uses): \"%s\"\n", shown + 1, count, tb.c_str());
        } else {
            auto tb = Buf {};
            tb.append(display_sv);
            printf("  #%zu (%zu uses): \"%s\"\n", shown + 1, count, tb.c_str());
        }
        ++shown;
    }

    auto unique_instruction_bytes = std::size_t { 0 };
    for (auto const& [iid, _] : instruction_usage) {
        unique_instruction_bytes += pup::global_pool().get(iid).size();
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
    auto fmt = global_pool().get(opts.show_format);
    if (fmt == "script") {
        return cmd_export_script(opts, variant_name);
    }
    if (fmt == "compdb") {
        return cmd_export_compdb(opts, variant_name);
    }
    if (fmt == "graph") {
        return cmd_export_graph(opts, variant_name);
    }
    if (fmt == "var") {
        return cmd_export_var(opts, variant_name);
    }
    if (fmt == "instructions" || fmt == "templates") {
        return cmd_export_instructions(opts, variant_name);
    }

    fprintf(stderr, "Unknown show format: %.*s\n", static_cast<int>(fmt.size()), fmt.data());
    fprintf(stderr, "Formats: script, compdb, graph, var, instructions\n");
    return EXIT_FAILURE;
}

} // namespace

auto cmd_show(Options const& opts) -> int
{
    if (is_empty(opts.show_format)) {
        fprintf(stderr, "Usage: putup show <format>\n");
        fprintf(stderr, "Formats: script, compdb, graph, var, instructions\n");
        return EXIT_FAILURE;
    }

    return for_each_variant(opts, show_single_variant, "Showing");
}

} // namespace pup::cli
