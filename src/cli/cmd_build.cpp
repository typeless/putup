// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/commands.hpp"
#include "pup/core/path.hpp"
#include "pup/cli/config_commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/multi_variant.hpp"
#include "pup/core/hash.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/node_id_map.hpp"
#include "pup/core/metrics.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/core/terminal.hpp"
#include "pup/core/types.hpp"
#include "pup/exec/progress_display.hpp"
#include "pup/exec/scheduler.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/rule_pattern.hpp"
#include "pup/index/entry.hpp"
#include "pup/index/writer.hpp"
#include "pup/platform/file_io.hpp"
#include "pup/platform/path.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <unordered_map>

namespace pup::cli {

namespace {

auto print_stats(
    pup::index::Index const& index,
    std::size_t num_commands,
    std::size_t commands_executed
) -> void
{
    auto metrics = pup::collect_metrics();

    auto implicit_deps_count = std::size_t { 0 };
    for (auto const& edge : index.edges()) {
        if (edge.type == pup::LinkType::Implicit) {
            ++implicit_deps_count;
        }
    }

    printf("\nStats:\n");
    printf("  Tupfiles parsed:    %6zu\n", metrics.tupfiles_parsed);
    printf("  Commands:           %6zu total, %zu executed\n", num_commands, commands_executed);
    printf("  Files checked:      %6zu (%zu changed)\n", metrics.files_checked, metrics.files_changed);
    printf("  Files in index:     %6zu\n", index.file_count());
    printf("  Edges in graph:     %6zu\n", index.edge_count());
    printf("  Implicit deps:      %6zu\n", implicit_deps_count);
    printf("  Hash computations:  %6zu\n", metrics.hash_computations);
    printf("  Hashes skipped:     %6zu (stat cache)\n", metrics.hashes_skipped);
    printf("  Stat calls:         %6zu\n", metrics.stat_calls);
    if (metrics.index_load_time.count() > 0 || metrics.index_save_time.count() > 0) {
        printf("  Index I/O:          %6ldms load, %ldms save\n", static_cast<long>(metrics.index_load_time.count()), static_cast<long>(metrics.index_save_time.count()));
    }

    // Phase timing breakdown (shown if any phase was timed)
    auto total_phase_us = metrics.command_index_time.count()
        + metrics.change_detection_time.count()
        + metrics.implicit_deps_time.count()
        + metrics.new_commands_time.count()
        + metrics.stale_outputs_time.count()
        + metrics.job_list_time.count();

    if (total_phase_us > 0) {
        printf("\n  Phase timing:\n");
        if (metrics.command_index_time.count() > 0) {
            printf("    Command index:    %6.1fms (%zu expansions)\n", metrics.command_index_time.count() / 1000.0, metrics.command_expansions);
        }
        if (metrics.change_detection_time.count() > 0) {
            printf("    Change detection: %6.1fms (%zu stats, %zu hashes, %zu skipped)\n", metrics.change_detection_time.count() / 1000.0, metrics.stat_calls, metrics.hash_computations, metrics.hashes_skipped);
        }
        if (metrics.implicit_deps_time.count() > 0) {
            printf("    Implicit deps:    %6.1fms\n", metrics.implicit_deps_time.count() / 1000.0);
        }
        if (metrics.new_commands_time.count() > 0) {
            printf("    New commands:     %6.1fms\n", metrics.new_commands_time.count() / 1000.0);
        }
        if (metrics.stale_outputs_time.count() > 0) {
            printf("    Stale outputs:    %6.1fms\n", metrics.stale_outputs_time.count() / 1000.0);
        }
        if (metrics.job_list_time.count() > 0) {
            printf("    Job list:         %6.1fms\n", metrics.job_list_time.count() / 1000.0);
        }
        printf("  Total overhead:     %6.1fms\n", total_phase_us / 1000.0);
    }
}

auto strip_build_root_prefix(std::string& path, std::string_view build_root_name) -> void
{
    if (!build_root_name.empty()) {
        auto prefix_len = build_root_name.size() + 1;
        if (path.size() > build_root_name.size() && path.starts_with(build_root_name) && path[build_root_name.size()] == '/') {
            path = path.substr(prefix_len);
        }
    }
}

template<typename... Args>
auto vprint(std::string_view variant_name, char const* fmt, Args&&... args) -> void
{
    printf("[%.*s] ", static_cast<int>(variant_name.size()), variant_name.data());
    if constexpr (sizeof...(args) == 0) {
        printf("%s", fmt);
    } else {
        printf(fmt, std::forward<Args>(args)...);
    }
}

template<typename... Args>
auto veprint(std::string_view variant_name, char const* fmt, Args&&... args) -> void
{
    fprintf(stderr, "[%.*s] ", static_cast<int>(variant_name.size()), variant_name.data());
    if constexpr (sizeof...(args) == 0) {
        fprintf(stderr, "%s", fmt);
    } else {
        fprintf(stderr, fmt, std::forward<Args>(args)...);
    }
}

auto is_tupfile(std::string_view path) -> bool
{
    return path.ends_with("/Tupfile") || path.ends_with("/Tuprules.tup")
        || path == "Tupfile" || path == "Tuprules.tup"
        || path.ends_with("/tup.config") || path == "tup.config";
}

/// Walk backward through the DAG from commands in scope, returning all
/// reachable nodes (the transitive upstream closure).
auto walk_upstream_from_scope(
    pup::graph::BuildGraph const& graph,
    std::vector<std::string> const& scopes
) -> std::vector<pup::NodeId>
{
    if (scopes.empty()) {
        return {};
    }

    auto visited = pup::NodeIdMap32 {};
    auto result = std::vector<pup::NodeId> {};
    auto stack = std::vector<pup::NodeId> {};

    // Seed with commands whose source_dir is in scope
    for (auto id : graph.all_nodes()) {
        if (!pup::node_id::is_command(id)) {
            continue;
        }
        auto const* node = graph.get_command_node(id);
        if (!node) {
            continue;
        }

        auto source_dir_sv = pup::graph::get_source_dir(graph.graph(), id);
        if (!pup::is_path_in_any_scope(std::string { source_dir_sv }, scopes)) {
            continue;
        }

        visited.set(id, 1);
        result.push_back(id);

        for (auto input_id : graph.get_inputs(id)) {
            stack.push_back(input_id);
        }
        for (auto dep_id : graph.get_order_only(id)) {
            stack.push_back(dep_id);
        }
    }

    while (!stack.empty()) {
        auto id = stack.back();
        stack.pop_back();

        if (visited.contains(id)) {
            continue;
        }
        visited.set(id, 1);
        result.push_back(id);

        for (auto input_id : graph.get_inputs(id)) {
            stack.push_back(input_id);
        }
        for (auto dep_id : graph.get_order_only(id)) {
            stack.push_back(dep_id);
        }
    }

    return result;
}

/// Collect all upstream input file paths for commands in the given scopes.
auto collect_upstream_files(
    pup::graph::BuildGraph const& graph,
    std::vector<std::string> const& scopes
) -> std::set<std::string>
{
    auto upstream = std::set<std::string> {};
    for (auto id : walk_upstream_from_scope(graph, scopes)) {
        if (pup::node_id::is_command(id)) {
            continue;
        }
        auto const* node = graph.get_file_node(id);
        if (node && (node->type == pup::NodeType::File || node->type == pup::NodeType::Generated)) {
            auto path = graph.get_full_path(id);
            if (!path.empty()) {
                upstream.insert(path);
            }
        }
    }
    return upstream;
}

/// Collect commands in scope plus all transitive upstream producer commands.
auto collect_scope_with_upstream_commands(
    pup::graph::BuildGraph const& graph,
    std::vector<std::string> const& scopes
) -> pup::NodeIdMap32
{
    auto commands = pup::NodeIdMap32 {};
    for (auto id : walk_upstream_from_scope(graph, scopes)) {
        if (pup::node_id::is_command(id) && graph.get_command_node(id)) {
            commands.set(id, 1);
        }
    }
    return commands;
}

auto find_changed_files_with_implicit(
    std::string const& source_root,
    pup::index::Index const& old_index,
    std::vector<std::string> const& scopes,
    std::set<std::string> const& upstream_files,
    bool verbose = false
) -> std::vector<std::string>
{
    auto changed = std::vector<std::string> {};
    auto& metrics = pup::thread_metrics();

    // Racy-clean threshold: files modified within 1 second of index save
    auto const save_time_ns = old_index.save_time_ns();
    auto constexpr RACY_CLEAN_THRESHOLD_NS = std::int64_t { 1'000'000'000 };

    for (auto const& file : old_index.files()) {
        if (file.type != pup::NodeType::File && file.type != pup::NodeType::Generated) {
            continue;
        }

        // Skip files outside scopes (but always check Tupfiles and upstream deps)
        if (!scopes.empty() && !is_tupfile(file.path)
            && !pup::is_path_in_any_scope(file.path, scopes)
            && !upstream_files.contains(file.path)) {
            continue;
        }

        ++metrics.files_checked;

        // File resolution:
        // All paths are now source-relative (generated files include build root, e.g., "build/program").
        auto file_path = std::string { file.path };
        auto path = pup::path::is_absolute(file_path) ? file_path : pup::path::join(source_root, file_path);
        ++metrics.stat_calls;
        auto stat_result = pup::platform::stat_file(path);

        if (!stat_result) {
            if (verbose) {
                printf("  Changed (stat failed): %s\n", file.path.c_str());
            }
            ++metrics.files_changed;
            changed.push_back(file.path);
            continue;
        }

        // Size check (fast path)
        auto current_size = stat_result->size;
        if (current_size != file.size) {
            if (verbose) {
                printf("  Changed (size): %s\n", file.path.c_str());
            }
            ++metrics.files_changed;
            changed.push_back(file.path);
            continue;
        }

        // Stat cache: skip hash if size + mtime match and not racy-clean
        auto const current_mtime_ns = stat_result->mtime_ns;
        auto const cached_mtime_ns = file.mtime_ns;
        auto const is_racy_clean = save_time_ns > 0
            && cached_mtime_ns >= save_time_ns - RACY_CLEAN_THRESHOLD_NS;

        if (cached_mtime_ns != 0 && current_mtime_ns == cached_mtime_ns && !is_racy_clean) {
            // Stat cache hit: size + mtime match, trust cached hash
            ++metrics.hashes_skipped;
            continue;
        }

        // Content hash check (authoritative)
        if (file.content_hash != pup::ZERO_HASH) {
            auto hash_result = pup::sha256_file(path);
            if (!hash_result || *hash_result != file.content_hash) {
                if (verbose) {
                    printf("  Changed (hash): %s\n", file.path.c_str());
                }
                ++metrics.files_changed;
                changed.push_back(file.path);
            }
        } else {
            // ZERO_HASH indicates hash wasn't computed - treat as changed to be safe
            if (verbose) {
                printf("  Changed (no hash): %s\n", file.path.c_str());
            }
            ++metrics.files_changed;
            changed.push_back(file.path);
        }
    }

    return changed;
}

/// Context for building implicit dependency entries in the index.
/// Holds mutable state shared between get_or_create_dir and create_implicit_file.
struct ImplicitDepContext {
    pup::index::Index& index;
    std::unordered_map<std::string, pup::NodeId>& path_to_id;
    pup::NodeId& next_id;
    std::set<std::pair<pup::NodeId, pup::NodeId>>& added_edges;
    std::string const& source_root;
};

/// Recursively get or create directory entries in the index.
/// Returns the NodeId for the directory at dir_path.
auto get_or_create_dir(
    ImplicitDepContext& ctx,
    std::string const& dir_path
) -> pup::NodeId
{
    auto normalized = pup::path::normalize(dir_path);
    auto path_str = normalized;

    if (path_str.empty() || path_str == ".") {
        return pup::NodeId { 0 };
    }

    if (auto it = ctx.path_to_id.find(path_str); it != ctx.path_to_id.end()) {
        return it->second;
    }

    if (path_str == "/") {
        auto dir_id = ctx.next_id++;
        auto entry = pup::index::FileEntry {
            .id = dir_id,
            .parent_id = pup::NodeId { 0 },
            .src_id = 0,
            .type = pup::NodeType::Directory,
            .flags = pup::NodeFlags::None,
            .name = "/",
            .path = "/",
            .size = 0,
            .mtime_ns = 0,
            .content_hash = {},
        };
        ctx.index.add_file(std::move(entry));
        ctx.path_to_id["/"] = dir_id;
        return dir_id;
    }

    auto parent_path = std::string { pup::path::parent(normalized) };
    auto parent_id = get_or_create_dir(ctx, parent_path);
    auto basename = std::string { pup::path::filename(normalized) };

    auto dir_id = ctx.next_id++;
    auto entry = pup::index::FileEntry {
        .id = dir_id,
        .parent_id = parent_id,
        .src_id = 0,
        .type = pup::NodeType::Directory,
        .flags = pup::NodeFlags::None,
        .name = basename,
        .path = path_str,
        .size = 0,
        .mtime_ns = 0,
        .content_hash = {},
    };
    ctx.index.add_file(std::move(entry));
    ctx.path_to_id[path_str] = dir_id;
    return dir_id;
}

/// Create a file entry for an implicit dependency (header file discovered by compiler).
/// Creates parent directories as needed and returns the file's NodeId.
auto create_implicit_file(
    ImplicitDepContext& ctx,
    std::string const& abs_path,
    std::string const& rel_path
) -> pup::NodeId
{
    auto content_hash = pup::Hash256 {};
    auto file_size = std::uint64_t { 0 };
    auto mtime_ns = std::int64_t { 0 };
    if (pup::platform::exists(abs_path)) {
        auto hash_result = pup::sha256_file(abs_path);
        if (hash_result) {
            content_hash = *hash_result;
        } else {
            fprintf(stderr, "Warning: Failed to hash file: %s\n", pup::platform::to_utf8(abs_path).c_str());
        }

        auto stat_result = pup::platform::stat_file(abs_path);
        if (stat_result) {
            file_size = stat_result->size;
            mtime_ns = stat_result->mtime_ns;
        }
    }

    auto fs_path = std::string { rel_path };
    auto parent_id = get_or_create_dir(ctx, std::string { pup::path::parent(fs_path) });
    auto basename = std::string { pup::path::filename(fs_path) };

    auto file_id = ctx.next_id++;

    auto entry = pup::index::FileEntry {
        .id = file_id,
        .parent_id = parent_id,
        .src_id = 0,
        .type = pup::NodeType::File,
        .flags = pup::NodeFlags::None,
        .name = basename,
        .path = rel_path,
        .size = file_size,
        .mtime_ns = mtime_ns,
        .content_hash = content_hash,
    };
    ctx.index.add_file(std::move(entry));
    ctx.path_to_id[rel_path] = file_id;
    return file_id;
}

/// Serialize file and directory nodes from the build graph to the index.
/// Returns the populated index and a path-to-id mapping for later use.
auto serialize_graph_nodes(
    pup::graph::BuildGraph const& graph,
    std::string const& source_root,
    std::string const& output_root
) -> std::pair<pup::index::Index, std::unordered_map<std::string, pup::NodeId>>
{
    auto index = pup::index::Index {};
    auto path_to_id = std::unordered_map<std::string, pup::NodeId> {};

    for (auto id : graph.all_nodes()) {
        if (pup::node_id::is_command(id)) {
            continue;
        }
        auto const* node = graph.get_file_node(id);
        if (!node) {
            continue;
        }

        if (node->type == pup::NodeType::File || node->type == pup::NodeType::Generated) {
            auto node_path = graph.get_full_path(id);
            if (node_path.empty()) {
                continue;
            }

            // For generated files, strip build root for filesystem path construction
            // (output_root already contains the build root, so we need just the relative part)
            auto fs_path = node_path;
            if (node->type == pup::NodeType::Generated) {
                strip_build_root_prefix(fs_path, graph.get_build_root_name());
            }

            auto file_path = (node->type == pup::NodeType::Generated)
                ? pup::path::join(output_root, fs_path)
                : pup::path::join(source_root, node_path);

            auto content_hash = pup::Hash256 {};
            auto file_size = std::uint64_t { 0 };
            auto mtime_ns = std::int64_t { 0 };

            if (pup::platform::exists(file_path)) {
                auto hash_result = pup::sha256_file(file_path);
                if (hash_result) {
                    content_hash = *hash_result;
                } else {
                    fprintf(stderr, "Warning: Failed to hash file: %s\n", pup::platform::to_utf8(file_path).c_str());
                }

                auto stat_result = pup::platform::stat_file(file_path);
                if (stat_result) {
                    file_size = stat_result->size;
                    mtime_ns = stat_result->mtime_ns;
                }
            }

            auto entry = pup::index::FileEntry {
                .id = id,
                .parent_id = node->parent_dir,
                .src_id = 0,
                .type = node->type,
                .flags = node->flags,
                .name = std::string { pup::graph::get_name(graph.graph(), id) },
                .path = node_path,
                .size = file_size,
                .mtime_ns = mtime_ns,
                .content_hash = content_hash,
            };
            index.add_file(std::move(entry));
            path_to_id[node_path] = id;
        } else if (node->type == pup::NodeType::Directory || node->type == pup::NodeType::GeneratedDir) {
            auto node_path = graph.get_full_path(id);

            auto node_name_sv = pup::graph::get_name(graph.graph(), id);
            auto entry_name = std::string { node_name_sv };

            auto entry = pup::index::FileEntry {
                .id = id,
                .parent_id = node->parent_dir,
                .src_id = 0,
                .type = node->type,
                .flags = node->flags,
                .name = entry_name,
                .path = node_path,
                .size = 0,
                .mtime_ns = 0,
                .content_hash = {},
            };
            index.add_file(std::move(entry));
            if (!node_path.empty()) {
                path_to_id[node_path] = id;
            }
        } else if (node->type == pup::NodeType::Variable
                   || node->type == pup::NodeType::Group
                   || node->type == pup::NodeType::Ghost) {
            // These node types must be in index to maintain consecutive ID sequence
            auto entry = pup::index::FileEntry {
                .id = id,
                .parent_id = node->parent_dir,
                .src_id = 0,
                .type = node->type,
                .flags = node->flags,
                .name = std::string { pup::graph::get_name(graph.graph(), id) },
                .path = {},
                .size = 0,
                .content_hash = (node->type == pup::NodeType::Variable) ? node->content_hash : pup::Hash256 {},
            };
            index.add_file(std::move(entry));
        }
    }

    return { std::move(index), std::move(path_to_id) };
}

/// Serialize command nodes from the build graph to the index.
/// v8: Store template + operands instead of fully-expanded command.
auto serialize_command_nodes(
    pup::graph::BuildGraph const& graph,
    pup::index::Index& index,
    std::unordered_map<std::string, pup::NodeId> const& path_to_id
) -> void
{
    for (auto id : graph.all_nodes()) {
        if (!pup::node_id::is_command(id)) {
            continue;
        }
        auto const* cmd = graph.get_command_node(id);
        if (!cmd) {
            continue;
        }

        // v8: instruction and operands are stored directly on CommandNode
        auto instruction_pattern = std::string { pup::graph::get_instruction_pattern(graph.graph(), id) };
        auto inputs = cmd->inputs;
        auto outputs = cmd->outputs;

        // Look up source_dir in path_to_id to get the directory NodeId
        auto source_dir_sv = pup::graph::get_source_dir(graph.graph(), id);
        auto dir_id = pup::NodeId { 0 };
        if (!source_dir_sv.empty()) {
            auto it = path_to_id.find(std::string { source_dir_sv });
            if (it != path_to_id.end()) {
                dir_id = it->second;
            }
        }

        auto entry = pup::index::CommandEntry {
            .id = id,
            .dir_id = dir_id,
            .instruction_pattern = std::move(instruction_pattern),
            .display = std::string { pup::graph::get_display_str(graph.graph(), id) },
            .env = {},
            .inputs = std::move(inputs),
            .outputs = std::move(outputs),
        };
        index.add_command(std::move(entry));
    }
}

/// Serialize edges from the build graph to the index.
auto serialize_edges(
    pup::graph::BuildGraph const& graph,
    pup::index::Index& index
) -> void
{
    for (auto const& edge : graph.edges()) {
        index.add_edge(pup::index::EdgeEntry {
            .from = edge.from,
            .to = edge.to,
            .type = edge.type,
            .group_cmd_id = edge.group_cmd_id,
        });
    }
}

/// Compute the next available NodeId after all existing nodes.
auto compute_next_id(pup::graph::BuildGraph const& graph) -> pup::NodeId
{
    auto max_file_id = pup::NodeId { 0 };
    for (auto id : graph.all_nodes()) {
        if (!pup::node_id::is_command(id) && id > max_file_id) {
            max_file_id = id;
        }
    }
    return pup::NodeId { max_file_id + 1 };
}

/// Process discovered implicit dependencies from compiler output.
/// Adds new file entries and implicit edges to the index.
auto process_implicit_deps(
    std::unordered_map<pup::NodeId, std::vector<std::string>> const& discovered_deps,
    ImplicitDepContext& ctx
) -> void
{
    for (auto const& [cmd_id, deps] : discovered_deps) {
        for (auto const& dep_path : deps) {
            auto dep_fs_path = std::string { dep_path };
            auto abs_path = pup::path::is_absolute(dep_fs_path) ? dep_fs_path : pup::path::join(ctx.source_root, dep_fs_path);

            auto rel_path = std::string {};
            if (pup::is_path_under(abs_path, ctx.source_root)) {
                rel_path = pup::path::relative(abs_path, ctx.source_root);
            } else {
                rel_path = abs_path;
            }

            auto it = ctx.path_to_id.find(rel_path);
            auto dep_id = it != ctx.path_to_id.end()
                ? it->second
                : create_implicit_file(ctx, abs_path, rel_path);

            auto edge_key = std::pair { dep_id, cmd_id };
            if (ctx.added_edges.insert(edge_key).second) {
                ctx.index.add_edge(pup::index::EdgeEntry {
                    .from = dep_id,
                    .to = cmd_id,
                    .type = pup::LinkType::Implicit,
                    .group_cmd_id = 0,
                });
            }
        }
    }
}

/// Preserve implicit edges from the old index for commands that weren't rebuilt.
auto preserve_old_implicit_edges(
    pup::index::Index const& old_index,
    std::unordered_map<pup::NodeId, std::vector<std::string>> const& discovered_deps,
    ImplicitDepContext& ctx
) -> void
{
    auto commands_with_new_deps = pup::NodeIdMap32 {};
    for (auto const& [cmd_id, _] : discovered_deps) {
        commands_with_new_deps.set(cmd_id, 1);
    }

    for (auto const& edge : old_index.edges()) {
        if (edge.type != pup::LinkType::Implicit) {
            continue;
        }

        if (commands_with_new_deps.contains(edge.to)) {
            continue;
        }

        auto const* old_file = old_index.find_file_by_id(edge.from);
        if (!old_file) {
            continue;
        }

        auto new_file_it = ctx.path_to_id.find(old_file->path);
        auto old_path = std::string { old_file->path };
        auto abs_path = pup::path::is_absolute(old_path) ? old_path : pup::path::join(ctx.source_root, old_path);
        auto new_from_id = new_file_it != ctx.path_to_id.end()
            ? new_file_it->second
            : create_implicit_file(ctx, abs_path, old_file->path);

        auto edge_key = std::pair { new_from_id, edge.to };
        if (ctx.added_edges.insert(edge_key).second) {
            ctx.index.add_edge(pup::index::EdgeEntry {
                .from = new_from_id,
                .to = edge.to,
                .type = pup::LinkType::Implicit,
                .group_cmd_id = 0,
            });
        }
    }
}

auto expand_implicit_deps(
    std::vector<std::string> const& changed,
    pup::index::Index const& index,
    pup::graph::BuildGraph const& graph
) -> std::vector<std::string>
{
    auto result = std::vector<std::string> { changed };
    auto added = std::set<std::string> { changed.begin(), changed.end() };

    auto path_to_file = std::unordered_map<std::string, pup::index::FileEntry const*> {};
    for (auto const& file : index.files()) {
        path_to_file[file.path] = &file;
    }

    // Build edge index: from_id -> vector of edges (O(edges) once)
    auto edges_by_from = std::unordered_map<pup::NodeId, std::vector<pup::index::EdgeEntry const*>> {};
    for (auto const& edge : index.edges()) {
        if (edge.type == pup::LinkType::Implicit || edge.type == pup::LinkType::Sticky) {
            edges_by_from[edge.from].push_back(&edge);
        }
    }

    // O(1) lookup per changed file
    for (auto const& path : changed) {
        auto it = path_to_file.find(path);
        if (it == path_to_file.end()) {
            continue;
        }

        auto file_id = pup::NodeId { it->second->id };
        auto edge_it = edges_by_from.find(file_id);
        if (edge_it == edges_by_from.end()) {
            continue;
        }

        for (auto const* edge : edge_it->second) {
            auto cmd_id = pup::NodeId { edge->to };
            auto const* cmd = index.find_command_by_id(cmd_id);
            if (!cmd) {
                continue;
            }

            // v8: Reconstruct command string from template + operands
            auto cmd_str = pup::index::get_command_string(index, *cmd);
            auto cmd_node_id = graph.find_by_command(cmd_str);
            if (!cmd_node_id) {
                continue;
            }

            for (auto output_id : graph.get_outputs(*cmd_node_id)) {
                auto output_path = graph.get_full_path(output_id);
                if (!output_path.empty()) {
                    if (added.insert(output_path).second) {
                        result.push_back(output_path);
                    }
                }
            }
        }
    }

    return result;
}

/// Build a complete index from the build graph and discovered dependencies.
/// Orchestrates the serialization of nodes, commands, edges, and implicit deps.
auto build_index(
    pup::graph::BuildGraph const& graph,
    std::unordered_map<pup::NodeId, std::vector<std::string>> const& discovered_deps,
    std::string const& source_root,
    std::string const& output_root,
    pup::index::Index const* old_index = nullptr
) -> pup::index::Index
{
    // Serialize file/directory nodes from the build graph
    auto [index, path_to_id] = serialize_graph_nodes(graph, source_root, output_root);

    // Serialize command nodes
    serialize_command_nodes(graph, index, path_to_id);

    // Serialize edges from the build graph
    serialize_edges(graph, index);

    // Setup context for implicit dependency processing
    auto next_id = compute_next_id(graph);
    auto added_edges = std::set<std::pair<pup::NodeId, pup::NodeId>> {};
    auto ctx = ImplicitDepContext {
        .index = index,
        .path_to_id = path_to_id,
        .next_id = next_id,
        .added_edges = added_edges,
        .source_root = source_root,
    };

    // Process discovered implicit dependencies from compiler output
    process_implicit_deps(discovered_deps, ctx);

    // Preserve implicit edges from the old index for commands that weren't rebuilt
    if (old_index) {
        preserve_old_implicit_edges(*old_index, discovered_deps, ctx);
    }

    return index;
}

/// Validate output targets exist in the build graph.
/// Returns node IDs on success, or empty optional with error printed on failure.
auto validate_output_targets(
    std::vector<std::string> const& targets,
    pup::graph::BuildGraph const& graph,
    std::string_view variant_name,
    bool verbose
) -> std::optional<std::vector<pup::NodeId>>
{
    auto node_ids = std::vector<pup::NodeId> {};
    for (auto const& target : targets) {
        auto node_id = graph.find_by_path(target, pup::BUILD_ROOT_ID);
        if (!node_id) {
            veprint(variant_name, "Error: %s is not in build graph\n", target.c_str());
            return std::nullopt;
        }
        auto const* node = graph.get_file_node(*node_id);
        if (!node || node->type != pup::NodeType::Generated) {
            veprint(variant_name, "Error: %s is not a build output\n", target.c_str());
            return std::nullopt;
        }
        node_ids.push_back(*node_id);
        if (verbose) {
            vprint(variant_name, "Output target: %s\n", target.c_str());
        }
    }
    return node_ids;
}

/// Detect new commands (in graph but not index) and add their outputs to changed files.
auto detect_new_commands(
    pup::graph::BuildGraph const& graph,
    pup::index::Index const& idx,
    std::string_view variant_name,
    bool verbose
) -> std::vector<std::string>
{
    auto changed = std::vector<std::string> {};
    for (auto id : graph.all_nodes()) {
        if (!pup::node_id::is_command(id)) {
            continue;
        }
        auto const* node = graph.get_command_node(id);
        if (!node) {
            continue;
        }

        auto cmd_str = graph.expand_instruction(id);
        auto found = idx.find_command_by_command(cmd_str);
        if (!found) {
            for (auto output_id : graph.get_outputs(id)) {
                auto output_path = graph.get_full_path(output_id);
                if (!output_path.empty()) {
                    changed.push_back(output_path);
                }
            }
            if (verbose) {
                auto display_sv = pup::graph::get_display_str(graph.graph(), id);
                vprint(variant_name, "  New command: %.*s\n", static_cast<int>(display_sv.size()), display_sv.data());
            }
        }
    }
    return changed;
}

/// Remove stale outputs from removed commands and report them.
auto remove_stale_outputs(
    pup::graph::BuildGraph const& graph,
    pup::index::Index const& idx,
    std::string const& source_root,
    std::string_view variant_name,
    bool dry_run,
    bool verbose
) -> void
{
    for (auto const& cmd : idx.commands()) {
        // v8: Reconstruct command string from template + operands
        auto cmd_str = pup::index::get_command_string(idx, cmd);
        if (graph.find_by_command(cmd_str)) {
            continue;
        }

        for (auto const* edge : idx.edges_from(cmd.id)) {
            auto const* file = idx.find_file_by_id(edge->to);
            if (!file || file->type != pup::NodeType::Generated) {
                continue;
            }

            // Paths now include build root (e.g., "build/program")
            auto abs_path = pup::path::join(source_root, file->path);
            if (pup::platform::exists(abs_path)) {
                if (dry_run) {
                    vprint(variant_name, "Would remove stale: %s\n", file->path.c_str());
                } else {
                    if (pup::platform::remove_file(abs_path)) {
                        if (verbose) {
                            vprint(variant_name, "  Removed stale: %s\n", file->path.c_str());
                        }
                    }
                }
            }
        }

        if (verbose) {
            vprint(variant_name, "  Removed command: %s\n", cmd.display.c_str());
        }
    }
}

/// Build mode precedence (highest to lowest):
/// 1. Incremental - if old index exists and files changed
/// 2. ScopeWithUpstream - fresh build with -a and explicit targets
/// 3. Targets - if specific output targets requested (and not incremental)
/// 4. Subset - exclude config commands from full build
/// 5. Full - build everything
enum class BuildMode {
    Incremental,
    ScopeWithUpstream,
    Targets,
    Subset,
    Full,
};

auto determine_build_mode(
    bool has_targets,
    bool use_incremental,
    bool has_config_cmds,
    bool scope_with_upstream
) -> BuildMode
{
    if (use_incremental) {
        return BuildMode::Incremental;
    }
    if (scope_with_upstream) {
        return BuildMode::ScopeWithUpstream;
    }
    if (has_targets) {
        return BuildMode::Targets;
    }
    if (has_config_cmds) {
        return BuildMode::Subset;
    }
    return BuildMode::Full;
}

/// Build a single variant with the given options.
/// Expects opts.build_dirs to contain at most one element.
auto build_single_variant(
    Options const& opts,
    std::string_view variant_name
) -> int
{
    auto scanner_registry = make_scanner_registry();
    auto* const scanner_ptr = scanner_registry ? &*scanner_registry : nullptr;
    if (scanner_ptr && opts.verbose) {
        vprint(variant_name, "Implicit dependency tracking enabled\n");
    }

    auto layout = discover_layout(make_layout_options(opts));
    if (!layout) {
        veprint(variant_name, "Error: %s\n", layout.error().message.c_str());
        return EXIT_FAILURE;
    }
    auto scopes = compute_build_scopes(opts, *layout);

    // Only scope parsing when explicit targets are given.
    // CWD-derived scoping should still parse all Tupfiles so that
    // out-of-scope Tupfile changes are detected for incremental builds.
    // When -a is set, always parse all Tupfiles so that cross-directory
    // producers are discovered and ghost nodes get resolved.
    auto parse_scopes = (opts.targets.empty() || opts.include_all_deps)
        ? std::vector<std::string> {}
        : scopes;

    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
        .keep_going = opts.keep_going,
        .auto_init = true,
        .root_config_only = false,
        .require_config = true,
        .parse_scopes = parse_scopes,
        .scanner_registry = scanner_ptr,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        veprint(variant_name, "Error: %s\n", result.error().message.c_str());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;
    auto num_commands = std::size_t { ctx.graph().nodes_of_type(pup::NodeType::Command).size() };

    if (num_commands == 0) {
        vprint(variant_name, "Nothing to do.\n");
        return EXIT_SUCCESS;
    }

    auto target_ids_result = validate_output_targets(
        opts.output_targets,
        ctx.graph(),
        variant_name,
        opts.verbose
    );
    if (!target_ids_result) {
        return EXIT_FAILURE;
    }
    auto target_node_ids = std::move(*target_ids_result);

    auto index_path = ctx.layout().index_path();
    auto const* old_idx_ptr = ctx.old_index();
    auto use_incremental = false;
    auto changed_files = std::vector<std::string> {};

    if (old_idx_ptr) {
        auto const& idx = *old_idx_ptr;

        // Build command string index for find_by_command() lookups
        // Must happen after parsing (operands set) but before incremental logic
        auto cmd_index_start = std::chrono::high_resolution_clock::now();
        ctx.graph().build_command_index();
        auto cmd_index_elapsed = std::chrono::high_resolution_clock::now() - cmd_index_start;
        pup::thread_metrics().command_index_time = std::chrono::duration_cast<std::chrono::microseconds>(cmd_index_elapsed);

        auto upstream_files = std::set<std::string> {};
        if (opts.include_all_deps && !scopes.empty()) {
            upstream_files = collect_upstream_files(ctx.graph(), scopes);
        }
        if (opts.verbose) {
            if (scopes.empty()) {
                vprint(variant_name, "Full project build\n");
            } else {
                vprint(variant_name, "Scoped build:");
                for (auto const& s : scopes) {
                    printf(" %s", s.c_str());
                }
                if (opts.include_all_deps) {
                    printf(" (+%zu upstream deps)", upstream_files.size());
                }
                printf("\n");
            }
        }

        auto change_detect_start = std::chrono::high_resolution_clock::now();
        changed_files = find_changed_files_with_implicit(
            ctx.layout().source_root,
            idx,
            scopes,
            upstream_files,
            opts.verbose
        );
        auto change_detect_elapsed = std::chrono::high_resolution_clock::now() - change_detect_start;
        pup::thread_metrics().change_detection_time = std::chrono::duration_cast<std::chrono::microseconds>(change_detect_elapsed);

        auto implicit_deps_start = std::chrono::high_resolution_clock::now();
        changed_files = expand_implicit_deps(changed_files, idx, ctx.graph());
        auto implicit_deps_elapsed = std::chrono::high_resolution_clock::now() - implicit_deps_start;
        pup::thread_metrics().implicit_deps_time = std::chrono::duration_cast<std::chrono::microseconds>(implicit_deps_elapsed);

        // Add output targets to force their rebuild
        // Output targets are source-relative (e.g., "hello"), but changed_files uses
        // full paths from get_full_path() which include build root prefix (e.g., "build-debug/hello").
        auto build_root_name = std::string { ctx.graph().get_build_root_name() };
        for (auto const& output_path : opts.output_targets) {
            auto prefixed = std::string {};
            if (!build_root_name.empty()) {
                prefixed.reserve(build_root_name.size() + 1 + output_path.size());
                prefixed.append(build_root_name).append("/").append(output_path);
            } else {
                prefixed = output_path;
            }
            if (std::ranges::find(changed_files, prefixed) == changed_files.end()) {
                changed_files.push_back(prefixed);
            }
        }

        auto new_cmds_start = std::chrono::high_resolution_clock::now();
        auto new_cmd_outputs = detect_new_commands(ctx.graph(), idx, variant_name, opts.verbose);
        auto new_cmds_elapsed = std::chrono::high_resolution_clock::now() - new_cmds_start;
        pup::thread_metrics().new_commands_time = std::chrono::duration_cast<std::chrono::microseconds>(new_cmds_elapsed);
        changed_files.insert(changed_files.end(), new_cmd_outputs.begin(), new_cmd_outputs.end());

        auto stale_start = std::chrono::high_resolution_clock::now();
        remove_stale_outputs(
            ctx.graph(),
            idx,
            ctx.layout().source_root,
            variant_name,
            opts.dry_run,
            opts.verbose
        );
        auto stale_elapsed = std::chrono::high_resolution_clock::now() - stale_start;
        pup::thread_metrics().stale_outputs_time = std::chrono::duration_cast<std::chrono::microseconds>(stale_elapsed);

        if (changed_files.empty()) {
            vprint(variant_name, "Nothing to do (up to date).\n");
            if (opts.stat) {
                print_stats(idx, num_commands, 0);
            }
            return EXIT_SUCCESS;
        }

        use_incremental = true;
        if (opts.verbose) {
            vprint(variant_name, "Incremental build: %zu changed files\n", changed_files.size());
        }
    }

    auto sched_opts = pup::exec::SchedulerOptions {
        .jobs = opts.jobs,
        .keep_going = opts.keep_going,
        .dry_run = opts.dry_run,
        .verbose = opts.verbose,
        .source_root = ctx.layout().source_root,
        .config_root = ctx.layout().config_root,
        .output_root = ctx.layout().output_root,
    };

    auto scheduler = pup::exec::Scheduler { sched_opts };
    auto discovered_deps = std::unordered_map<pup::NodeId, std::vector<std::string>> {};
    auto deps_mutex = std::mutex {};
    auto progress_mutex = std::mutex {};

    auto use_tty_progress = pup::stdout_is_tty() && !opts.verbose && !opts.dry_run;
    auto progress = pup::exec::ProgressState { .total = num_commands };
    auto prev_lines = std::size_t { 0 };

    scheduler.on_job_start([&](pup::exec::BuildJob const& job) {
        if (opts.verbose || opts.dry_run) {
            vprint(variant_name, "%s\n", job.display.c_str());
        } else if (use_tty_progress) {
            auto lock = std::lock_guard { progress_mutex };
            auto target = job.outputs.empty() ? job.display : job.outputs.front();
            progress = pup::exec::job_started(std::move(progress), job.id, target);
            auto output = pup::exec::render_tty(progress, variant_name);
            pup::exec::display_progress(output, prev_lines);
        }
    });

    scheduler.on_job_complete([&](pup::exec::BuildJob const& job, pup::exec::JobResult const& job_result) {
        if (!job_result.success) {
            if (use_tty_progress) {
                auto lock = std::lock_guard { progress_mutex };
                pup::exec::finalize_progress(prev_lines);
            }
            veprint(variant_name, "FAILED: %s\n", job.display.c_str());
            if (!job_result.output.empty()) {
                fprintf(stderr, "%s\n", job_result.output.c_str());
            }
        }

        if (!job_result.discovered_deps.empty()) {
            auto lock = std::lock_guard { deps_mutex };
            auto target_id = job_result.deps_for_command != pup::INVALID_NODE_ID
                ? job_result.deps_for_command
                : job.id;
            auto& deps = discovered_deps[target_id];

            for (auto const& dep_path : job_result.discovered_deps) {
                auto to_resolve = pup::path::is_absolute(dep_path)
                    ? dep_path
                    : pup::path::join(job.working_dir, dep_path);
                auto resolved_result = pup::platform::canonical(to_resolve);
                if (!resolved_result) {
                    if (opts.verbose) {
                        fprintf(stderr, "Warning: Skipping dependency '%s': %s\n", dep_path.c_str(), resolved_result.error().message.c_str());
                    }
                    continue;
                }
                auto resolved = *resolved_result;

                if (pup::is_path_under(resolved, ctx.layout().source_root)) {
                    auto rel = pup::path::relative(resolved, ctx.layout().source_root);
                    if (rel.starts_with("..")) {
                        if (opts.verbose) {
                            fprintf(stderr, "Warning: Cannot relativize '%s'\n", resolved.c_str());
                        }
                        continue;
                    }
                    deps.push_back(rel);
                } else {
                    deps.push_back(resolved);
                }
            }
        }

        if (use_tty_progress) {
            auto lock = std::lock_guard { progress_mutex };
            progress = pup::exec::job_completed(std::move(progress), job.id, job_result.success);
            auto output = pup::exec::render_tty(progress, variant_name);
            pup::exec::display_progress(output, prev_lines);
        } else if (!opts.verbose && !opts.dry_run) {
            auto lock = std::lock_guard { progress_mutex };
            progress = pup::exec::job_completed(std::move(progress), job.id, job_result.success);
            printf("\r%s ", pup::exec::render_simple(progress, variant_name).c_str());
            std::fflush(stdout);
        }
    });

    scheduler.on_progress([&](std::size_t /* done */, std::size_t total) {
        auto lock = std::lock_guard { progress_mutex };
        progress.total = total;
    });

    // Identify config-generating commands to exclude from regular build
    // (config rules should only run during 'pup configure')
    auto config_cmds = find_config_commands(ctx.graph(), ctx.layout().source_root);
    auto config_cmd_ids = NodeIdMap32 {};
    for (auto const& cfg : config_cmds) {
        config_cmd_ids.set(cfg.cmd_id, 1);
    }

    auto start = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
    auto build_result = pup::Result<pup::exec::BuildStats> {};

    auto scope_with_upstream = opts.include_all_deps && !scopes.empty() && !use_incremental;
    auto mode = determine_build_mode(
        !target_node_ids.empty(),
        use_incremental,
        !config_cmds.empty(),
        scope_with_upstream
    );

    switch (mode) {
    case BuildMode::Incremental:
        build_result = scheduler.build_incremental(ctx.graph(), changed_files);
        break;
    case BuildMode::ScopeWithUpstream: {
        auto all_scope_cmds = collect_scope_with_upstream_commands(ctx.graph(), scopes);
        auto scope_cmds = pup::NodeIdMap32 {};
        for (auto id : ctx.graph().all_nodes()) {
            if (pup::node_id::is_command(id) && all_scope_cmds.contains(id) && !config_cmd_ids.contains(id)) {
                scope_cmds.set(id, 1);
            }
        }
        build_result = scheduler.build_subset(ctx.graph(), scope_cmds);
        break;
    }
    case BuildMode::Targets:
        build_result = scheduler.build_targets(ctx.graph(), target_node_ids);
        break;
    case BuildMode::Subset: {
        auto non_config_cmds = pup::NodeIdMap32 {};
        for (auto id : ctx.graph().all_nodes()) {
            if (node_id::is_command(id) && !config_cmd_ids.contains(id)) {
                non_config_cmds.set(id, 1);
            }
        }
        build_result = scheduler.build_subset(ctx.graph(), non_config_cmds);
        break;
    }
    case BuildMode::Full:
        build_result = scheduler.build(ctx.graph());
        break;
    }
    auto end = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
    auto duration = std::chrono::milliseconds { std::chrono::duration_cast<std::chrono::milliseconds>(end - start) };

    if (use_tty_progress) {
        pup::exec::finalize_progress(prev_lines);
    } else if (!opts.verbose && !opts.dry_run) {
        printf("\n");
    }

    if (!build_result) {
        veprint(variant_name, "Build failed: %s\n", build_result.error().message.c_str());
        return EXIT_FAILURE;
    }

    auto const& stats = *build_result;
    if (stats.completed_jobs == 0 && stats.failed_jobs == 0) {
        vprint(variant_name, "Nothing to do.\n");
    } else if (stats.failed_jobs > 0) {
        vprint(variant_name, "Build completed: %zu commands (%zu failed) in %ldms\n", stats.completed_jobs, stats.failed_jobs, static_cast<long>(duration.count()));
    } else {
        vprint(variant_name, "Build completed: %zu commands in %ldms\n", stats.completed_jobs, static_cast<long>(duration.count()));
    }

    auto final_index = std::optional<pup::index::Index> {};
    if (!opts.dry_run) {
        // Save index even after partial failures - successful outputs are recorded
        // so they won't be rebuilt. Failed outputs don't exist, so stat will fail
        // and they'll be detected as changed on next build.
        auto index = pup::index::Index { build_index(
            ctx.graph(),
            discovered_deps,
            ctx.layout().source_root,
            ctx.layout().output_root,
            old_idx_ptr
        ) };

        auto index_save_start = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
        auto write_result = pup::Result<void> { pup::index::write_index(index_path, index) };
        auto index_save_end = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
        pup::thread_metrics().index_save_time = std::chrono::duration_cast<std::chrono::milliseconds>(index_save_end - index_save_start);

        if (!write_result) {
            veprint(variant_name, "Warning: Failed to save index: %s\n", write_result.error().message.c_str());
        } else if (opts.verbose) {
            vprint(variant_name, "Saved index: %zu files, %zu commands, %zu edges\n", index.file_count(), index.command_count(), index.edge_count());
        }
        final_index = std::move(index);
    }

    if (opts.stat) {
        if (final_index) {
            print_stats(*final_index, num_commands, stats.completed_jobs);
        } else if (old_idx_ptr) {
            print_stats(*old_idx_ptr, num_commands, stats.completed_jobs);
        }
    }

    return stats.failed_jobs > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

} // anonymous namespace

auto cmd_build(Options const& opts) -> int
{
    return for_each_variant(opts, build_single_variant, "Building");
}

} // namespace pup::cli
