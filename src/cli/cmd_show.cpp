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
#include "pup/core/print.hpp"
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
#include <array>
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
        eprint("Warning: No index found - run 'putup' first\n");
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
        .excludes = make_exclude_list(opts),
        .scanner_registry = scanner_registry ? &*scanner_registry : nullptr,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        eprint("[{}] Error: {}\n", variant_name, result.error().msg());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;
    auto topo = pup::graph::topological_sort(ctx.graph().graph);
    auto const& cfg = ctx.config_vars();

    auto script_prologue = cfg.get("SCRIPT_PROLOGUE");
    if (script_prologue.empty()) {
        eprint("[{}] Error: SCRIPT_PROLOGUE not set in config\n", variant_name);
        return EXIT_FAILURE;
    }

    auto script_run = cfg.get("SCRIPT_RUN");
    if (script_run.empty()) {
        eprint("[{}] Error: SCRIPT_RUN not set in config\n", variant_name);
        return EXIT_FAILURE;
    }

    auto script_mkdir = cfg.get("SCRIPT_MKDIR");
    if (script_mkdir.empty()) {
        eprint("[{}] Error: SCRIPT_MKDIR not set in config\n", variant_name);
        return EXIT_FAILURE;
    }

    auto script_comment = cfg.get("SCRIPT_COMMENT");
    if (script_comment.empty()) {
        eprint("[{}] Error: SCRIPT_COMMENT not set in config\n", variant_name);
        return EXIT_FAILURE;
    }

    print("{}\n\n", script_prologue);

    auto output_dirs = Vec<StringId> {};
    for (auto id : graph::all_nodes(ctx.graph().graph)) {
        auto node_type = graph::get<pup::NodeType>(ctx.graph().graph, id);
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

    // StringId order is intern order, which varies with loaded-index state
    std::sort(output_dirs.begin(), output_dirs.end(), [](StringId a, StringId b) {
        return global_pool().get(a) < global_pool().get(b);
    });
    output_dirs.erase(std::unique(output_dirs.begin(), output_dirs.end()), output_dirs.end());

    if (!output_dirs.empty()) {
        print("{} Create output directories\n", script_comment);
        for (auto dir_id : output_dirs) {
            auto line = expand_script_run(script_mkdir, global_pool().get(dir_id), "");
            auto line_sv = global_pool().get(line);
            print("{}\n", line_sv);
        }
        print("\n");
    }

    for (auto id : topo.order) {
        if (!node_id::is_command(id)) {
            continue;
        }
        if (graph::get<graph::OutputAction>(ctx.graph().graph, id) == graph::OutputAction::InjectImplicitDeps) {
            continue;
        }
        if (!graph::is_guard_satisfied(ctx.graph().graph, id)) {
            continue;
        }

        auto source_dir = global_pool().get(graph::get<graph::SourceDir>(ctx.graph().graph, id));
        auto dir = source_dir.empty() ? std::string_view { "." } : source_dir;
        auto cmd_id = graph::expand_instruction(ctx.graph().graph, id);

        auto line = expand_script_run(script_run, dir, global_pool().get(cmd_id));
        auto line_sv = global_pool().get(line);
        print("{}\n", line_sv);
    }

    auto script_epilogue = cfg.get("SCRIPT_EPILOGUE");
    if (!script_epilogue.empty()) {
        print("\n{}\n", script_epilogue);
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
        .excludes = make_exclude_list(opts),
        .scanner_registry = scanner_registry ? &*scanner_registry : nullptr,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        eprint("[{}] Error: {}\n", variant_name, result.error().msg());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;
    auto index = load_index_for_all_deps(opts, ctx.layout());

    if (opts.summary) {
        auto commands = graph::nodes_of_type(ctx.graph().graph, pup::NodeType::Command);
        print("[{}] Tupfiles: {}\n", variant_name, ctx.parsed_dirs().size());
        print("[{}] Nodes: {}\n", variant_name, graph::node_count(ctx.graph().graph));
        print("[{}] Edges: {}\n", variant_name, graph::edge_count(ctx.graph().graph));
        print("[{}] Commands: {}\n", variant_name, commands.size());

        if (index) {
            auto implicit_count = std::size_t { 0 };
            for (auto const& edge : index->edges()) {
                if (edge.type == pup::LinkType::Implicit) {
                    ++implicit_count;
                }
            }
            print("[{}] Implicit edges: {}\n", variant_name, implicit_count);
        }

        if (opts.verbose) {
            print("[{}] Commands:\n", variant_name);
            for (auto id : commands) {
                auto display = global_pool().get(graph::command_label(ctx.graph().graph, id));
                print("[{}]   {}\n", variant_name, display);
            }
        }
        return EXIT_SUCCESS;
    }

    print("digraph G {\n");
    print("  rankdir=LR;\n");

    auto declared_nodes = pup::NodeIdMap32 {};
    for (auto id : graph::all_nodes(ctx.graph().graph)) {
        declared_nodes.set(id, 1);

        auto get_label = [&]() -> std::string_view {
            if (node_id::is_command(id)) {
                return pool.get(graph::command_label(ctx.graph().graph, id));
            }
            return pool.get(graph::get_full_path(ctx.graph().graph, id));
        };
        auto label = escape_dot_label(get_label());

        print("  {} [label=\"{}\"];\n", format_node_id(id), pool.get(label));

        for (auto input_id : graph::get_inputs(ctx.graph().graph, id)) {
            print("  {} -> {};\n", format_node_id(input_id), format_node_id(id));
        }

        // Output order-only edges (dotted)
        for (auto oo_id : graph::get_order_only(ctx.graph().graph, id)) {
            print("  {} -> {} [style=dotted color=\"#0088ff\"];\n", format_node_id(oo_id), format_node_id(id));
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
                    print("  {} [label=\"{}\" style=filled fillcolor=\"#f0f0f0\"];\n", format_node_id(from_id), pool.get(label_id));
                }
            }

            print("  {} -> {} [style=dashed color=\"#888888\"];\n", format_node_id(from_id), format_node_id(to_id));
        }
    }

    print("}\n");
    return EXIT_SUCCESS;
}

auto cmd_export_compdb(Options const& opts, std::string_view variant_name) -> int
{
    auto& pool = global_pool();
    auto scanner_registry = make_scanner_registry();
    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
        .require_config = true,
        .excludes = make_exclude_list(opts),
        .scanner_registry = scanner_registry ? &*scanner_registry : nullptr,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        eprint("[{}] Error: {}\n", variant_name, result.error().msg());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;

    print("[\n");
    auto commands = graph::nodes_of_type(ctx.graph().graph, pup::NodeType::Command);
    auto first = true;

    for (auto id : commands) {
        if (!graph::is_guard_satisfied(ctx.graph().graph, id)) {
            continue;
        }
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
        auto source_dir_sv = global_pool().get(graph::get<graph::SourceDir>(ctx.graph().graph, id));
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
            print(",\n");
        }
        first = false;

        print("  {\n");
        print("    \"directory\": \"{}\",\n", pool.get(escape_json(working_dir)));

        print("    \"arguments\": [");
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i > 0) {
                print(", ");
            }
            print("\"{}\"", pool.get(escape_json(pool.get(args[i]))));
        }
        print("],\n");

        print("    \"file\": \"{}\"", pool.get(escape_json(source_rel)));
        if (!output_rel.empty()) {
            print(",\n    \"output\": \"{}\"", pool.get(escape_json(output_rel)));
        }
        print("\n  }");
    }

    print("\n]\n");
    return EXIT_SUCCESS;
}

auto output_var_text(
    Vec<parser::VarHistory> const& histories
) -> int
{
    for (auto const& history : histories) {
        print("{} = {}\n", global_pool().get(history.name), global_pool().get(history.final_value));
        print("  History:\n");
        for (auto const* assign : history.assignments) {
            auto const* prefix = assign->is_effective ? "  " : "# ";
            auto op_str = parser::op_to_string(assign->op);
            print("  {}{}:{}\t{} {} {}", prefix, global_pool().get(assign->filename), assign->line, global_pool().get(assign->name), op_str, global_pool().get(assign->value_after));
            if (!assign->is_effective) {
                print("   (ineffective)");
            }
            print("\n");
        }
        print("\n");
    }
    return EXIT_SUCCESS;
}

auto output_var_json(
    Vec<parser::VarHistory> const& histories,
    std::string_view variant_name
) -> int
{
    auto& pool = global_pool();
    print("{\n");
    print("  \"variant\": \"{}\",\n", variant_name);
    print("  \"variables\": {\n");

    auto first_var = true;
    for (auto const& history : histories) {
        if (!first_var) {
            print(",\n");
        }
        first_var = false;

        print("    \"{}\": {{\n", pool.get(escape_json(global_pool().get(history.name))));
        print("      \"value\": \"{}\",\n", pool.get(escape_json(global_pool().get(history.final_value))));
        print("      \"history\": [\n");

        auto first_assign = true;
        for (auto const* assign : history.assignments) {
            if (!first_assign) {
                print(",\n");
            }
            first_assign = false;

            auto op_str = parser::op_to_string(assign->op);
            print("        {\n");
            print("          \"file\": \"{}\",\n", pool.get(escape_json(global_pool().get(assign->filename))));
            print("          \"line\": {},\n", assign->line);
            print("          \"op\": \"{}\",\n", op_str);
            print("          \"value\": \"{}\",\n", pool.get(escape_json(global_pool().get(assign->value_after))));
            print("          \"effective\": {}\n", assign->is_effective ? "true" : "false");
            print("        }");
        }

        print("\n      ]\n");
        print("    }");
    }

    print("\n  }\n");
    print("}\n");
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
        eprint("[{}] Error: {}\n", variant_name, result.error().msg());
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
        eprint("[{}] Error: {}\n", variant_name, result.error().msg());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;
    auto const& graph = ctx.graph().graph;

    // Build instruction usage map: instruction_id -> list of command IDs using it
    auto instruction_usage = Vec<std::pair<StringId, Vec<NodeId>>> {};

    for (auto const& cmd : graph.commands) {
        if (!is_empty(cmd.instruction_id)) {
            auto pos = std::lower_bound(instruction_usage.begin(), instruction_usage.end(), cmd.instruction_id, [](auto const& p, auto const& k) { return pup::handle_less(p.first, k); });
            if (pos != instruction_usage.end() && pos->first == cmd.instruction_id) {
                pos->second.push_back(cmd.id);
            } else {
                instruction_usage.insert(pos, std::pair<StringId, Vec<NodeId>> { cmd.instruction_id, { cmd.id } });
            }
        }
    }

    auto total_commands = graph.commands.size();
    auto unique_instructions = instruction_usage.size();

    print("Instruction Analysis:\n");
    print("  Commands: {}\n", total_commands);
    print("  Unique instructions: {}\n", unique_instructions);
    if (unique_instructions > 0) {
        print("  Deduplication ratio: {}x\n", Fixed { double(total_commands) / double(unique_instructions), 1 });
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
    print("\nTop instructions:\n");
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
            print("  #{} ({} uses): \"{}\"\n", shown + 1, count, tb.c_str());
        } else {
            auto tb = Buf {};
            tb.append(display_sv);
            print("  #{} ({} uses): \"{}\"\n", shown + 1, count, tb.c_str());
        }
        ++shown;
    }

    auto unique_instruction_bytes = std::size_t { 0 };
    for (auto const& [iid, _] : instruction_usage) {
        unique_instruction_bytes += pup::global_pool().get(iid).size();
    }

    auto per_command_overhead = total_commands * 8; // instruction_id + operand offset
    print("\nStorage:\n");
    print("  Unique instruction strings: {} bytes\n", unique_instruction_bytes);
    print("  Per-command overhead: {} bytes ({} commands x 8)\n", per_command_overhead, total_commands);
    print("  Total: {} bytes\n", unique_instruction_bytes + per_command_overhead);

    return EXIT_SUCCESS;
}

auto link_type_name(pup::LinkType t) -> char const*
{
    switch (t) {
    case pup::LinkType::Normal:
        return "Normal";
    case pup::LinkType::Sticky:
        return "Sticky";
    case pup::LinkType::Group:
        return "Group";
    case pup::LinkType::Implicit:
        return "Implicit";
    case pup::LinkType::OrderOnly:
        return "OrderOnly";
    }
    return "?";
}

auto cmd_export_index(Options const& opts, std::string_view variant_name) -> int
{
    auto& pool = global_pool();

    auto layout_result = pup::discover_layout(make_layout_options(opts));
    if (!layout_result) {
        eprint("[{}] Error: {}\n", variant_name, layout_result.error().msg());
        return EXIT_FAILURE;
    }
    auto& layout = *layout_result;

    auto index_path_sv = pool.get(layout.index_path());
    if (!pup::platform::exists(index_path_sv)) {
        eprint("[{}] Error: No index found at {} — run 'putup' first\n", variant_name, index_path_sv);
        return EXIT_FAILURE;
    }

    auto index_result = pup::index::read_index(index_path_sv);
    if (!index_result) {
        eprint("[{}] Error reading index: {}\n", variant_name, index_result.error().msg());
        return EXIT_FAILURE;
    }
    auto& index = *index_result;

    auto edges_by_type = std::array<std::size_t, 6> {};
    for (auto const& e : index.edges()) {
        auto t = static_cast<std::size_t>(e.type);
        if (t < edges_by_type.size()) {
            ++edges_by_type[t];
        }
    }

    print("[{}] Index: {}\n", variant_name, index_path_sv);
    print("  Files:    {}\n", index.files().size());
    print("  Commands: {}\n", index.commands().size());
    print("  Edges:    {}", index.edge_count());
    auto first = true;
    for (auto t : { pup::LinkType::Normal, pup::LinkType::Sticky, pup::LinkType::Group, pup::LinkType::Implicit, pup::LinkType::OrderOnly }) {
        auto count = edges_by_type[static_cast<std::size_t>(t)];
        if (count > 0) {
            print("{}{}={}", first ? " (" : ", ", link_type_name(t), count);
            first = false;
        }
    }
    if (!first) {
        print(")");
    }
    print("\n");

    if (opts.summary) {
        auto cmds_with_implicit = std::size_t { 0 };
        for (auto const& cmd : index.commands()) {
            for (auto const& e : index.edges()) {
                if (e.to == cmd.id
                    && (e.type == pup::LinkType::Implicit || e.type == pup::LinkType::Sticky)) {
                    ++cmds_with_implicit;
                    break;
                }
            }
        }
        print("  Commands with implicit/sticky deps: {}/{}\n", cmds_with_implicit, index.commands().size());
        return EXIT_SUCCESS;
    }

    print("\nCommands (with implicit/sticky edges):\n");
    auto filter_sv = pool.get(opts.show_var_filter);
    for (auto const& cmd : index.commands()) {
        auto cmd_str_id = pup::index::get_command_string(index, cmd);
        auto cmd_sv = pool.get(cmd_str_id);

        if (!filter_sv.empty() && cmd_sv.find(filter_sv) == std::string_view::npos) {
            continue;
        }

        auto dir_sv = std::string_view {};
        if (auto const* dir = index.find_file_by_id(cmd.dir_id)) {
            dir_sv = pool.get(dir->path);
        }

        print("  c{}  [{}]\n", cmd.id, dir_sv);
        print("       cmd: {}\n", cmd_sv);

        auto implicit = Vec<StringId> {};
        auto sticky = Vec<StringId> {};
        for (auto const& e : index.edges()) {
            if (e.to != cmd.id) {
                continue;
            }
            if (e.type != pup::LinkType::Implicit && e.type != pup::LinkType::Sticky) {
                continue;
            }
            auto const* from = index.find_file_by_id(e.from);
            if (!from) {
                continue;
            }
            if (e.type == pup::LinkType::Implicit) {
                implicit.push_back(from->path);
            } else {
                sticky.push_back(from->path);
            }
        }

        if (implicit.empty() && sticky.empty()) {
            print("       (no implicit/sticky deps)\n");
        }
        for (auto p : implicit) {
            auto p_sv = pool.get(p);
            print("       implicit: {}\n", p_sv);
        }
        for (auto p : sticky) {
            auto p_sv = pool.get(p);
            print("       sticky:   {}\n", p_sv);
        }
    }

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
    if (fmt == "index") {
        return cmd_export_index(opts, variant_name);
    }

    eprint("Unknown show format: {}\n", fmt);
    eprint("Formats: script, compdb, graph, var, instructions, index\n");
    return EXIT_FAILURE;
}

} // namespace

auto cmd_show(Options const& opts) -> int
{
    if (is_empty(opts.show_format)) {
        eprint("Usage: putup show <format>\n");
        eprint("Formats: script, compdb, graph, var, instructions, index\n");
        return EXIT_FAILURE;
    }

    return for_each_variant(opts, show_single_variant, "Showing");
}

} // namespace pup::cli
