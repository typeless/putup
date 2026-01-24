// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/config_commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/multi_variant.hpp"
#include "pup/core/hash.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/metrics.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/core/terminal.hpp"
#include "pup/core/types.hpp"
#include "pup/core/y_combinator.hpp"
#include "pup/exec/progress_display.hpp"
#include "pup/exec/scheduler.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/rule_pattern.hpp"
#include "pup/index/entry.hpp"
#include "pup/index/writer.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <unordered_map>

namespace pup::cli {

namespace {

auto resolve_path(
    std::filesystem::path const& path,
    std::filesystem::path const& root
) -> std::filesystem::path
{
    return path.is_absolute() ? path : root / path;
}

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
    printf("  Stat calls:         %6zu\n", metrics.stat_calls);
    if (metrics.index_load_time.count() > 0 || metrics.index_save_time.count() > 0) {
        printf("  Index I/O:          %6ldms load, %ldms save\n", static_cast<long>(metrics.index_load_time.count()), static_cast<long>(metrics.index_save_time.count()));
    }
}

auto is_tupfile(std::string_view path) -> bool
{
    return path.ends_with("/Tupfile") || path.ends_with("/Tuprules.tup")
        || path == "Tupfile" || path == "Tuprules.tup"
        || path.ends_with("/tup.config") || path == "tup.config";
}

/// Collect all upstream input files for commands in the given scopes.
/// Walks backwards through the DAG from commands to their transitive inputs.
auto collect_upstream_files(
    pup::graph::BuildGraph const& graph,
    std::vector<std::string> const& scopes
) -> std::set<std::string>
{
    if (scopes.empty()) {
        return {};
    }

    auto upstream = std::set<std::string> {};
    auto visited = std::set<pup::NodeId> {};

    // Recursive helper to collect inputs
    auto collect = pup::YCombinator { [&](auto const& self, pup::NodeId id) -> void {
        if (!visited.insert(id).second) {
            return;
        }

        if (!pup::is_command_id(id)) {
            auto const* node = graph.get_file_node(id);
            if (node && (node->type == pup::NodeType::File || node->type == pup::NodeType::Generated)) {
                auto path = graph.get_full_path(id);
                if (!path.empty()) {
                    upstream.insert(path);
                }
            }
        }

        // Walk to inputs (upstream)
        for (auto input_id : graph.get_inputs(id)) {
            self(input_id);
        }

        // Also follow order-only deps
        for (auto dep_id : graph.get_order_only(id)) {
            self(dep_id);
        }
    } };

    // Find commands in scope and collect their upstream deps
    for (auto id : graph.all_nodes()) {
        if (!pup::is_command_id(id)) {
            continue;
        }
        auto const* node = graph.get_command_node(id);
        if (!node) {
            continue;
        }

        // Check if command's source_dir is in any scope
        auto source_dir_sv = pup::graph::get_source_dir(graph.graph(), id);
        if (!pup::is_path_in_any_scope(std::string { source_dir_sv }, scopes)) {
            continue;
        }

        // Collect all inputs for this command
        for (auto input_id : graph.get_inputs(id)) {
            collect(input_id);
        }
        for (auto dep_id : graph.get_order_only(id)) {
            collect(dep_id);
        }
    }

    return upstream;
}

auto find_changed_files_with_implicit(
    std::filesystem::path const& source_root,
    std::filesystem::path const& output_root,
    pup::index::Index const& old_index,
    std::vector<std::string> const& scopes,
    std::set<std::string> const& upstream_files,
    bool verbose = false
) -> std::vector<std::string>
{
    auto changed = std::vector<std::string> {};
    auto& metrics = pup::thread_metrics();

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
        // Paths are source-relative. Generated files exist at output_root,
        // source files exist at source_root.
        auto const& root = (file.type == pup::NodeType::Generated) ? output_root : source_root;
        auto path = resolve_path(file.path, root);
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
    std::filesystem::path const& source_root;
};

/// Recursively get or create directory entries in the index.
/// Returns the NodeId for the directory at dir_path.
auto get_or_create_dir(
    ImplicitDepContext& ctx,
    std::filesystem::path const& dir_path
) -> pup::NodeId
{
    auto normalized = dir_path.lexically_normal();
    auto path_str = normalized.string();

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
            .content_hash = {},
        };
        ctx.index.add_file(std::move(entry));
        ctx.path_to_id["/"] = dir_id;
        return dir_id;
    }

    auto parent_path = normalized.parent_path();
    auto parent_id = get_or_create_dir(ctx, parent_path);
    auto basename = normalized.filename().string();

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
    std::filesystem::path const& abs_path,
    std::string const& rel_path
) -> pup::NodeId
{
    auto content_hash = pup::Hash256 {};
    auto file_size = std::uint64_t { 0 };
    if (std::filesystem::exists(abs_path)) {
        auto hash_result = pup::sha256_file(abs_path);
        if (hash_result) {
            content_hash = *hash_result;
        } else {
            fprintf(stderr, "Warning: Failed to hash file: %s\n", abs_path.c_str());
        }

        auto ec = std::error_code {};
        file_size = std::filesystem::file_size(abs_path, ec);
    }

    auto fs_path = std::filesystem::path { rel_path };
    auto parent_id = get_or_create_dir(ctx, fs_path.parent_path());
    auto basename = fs_path.filename().string();

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
    std::filesystem::path const& source_root,
    std::filesystem::path const& output_root
) -> std::pair<pup::index::Index, std::unordered_map<std::string, pup::NodeId>>
{
    auto index = pup::index::Index {};
    auto path_to_id = std::unordered_map<std::string, pup::NodeId> {};

    for (auto id : graph.all_nodes()) {
        if (pup::is_command_id(id)) {
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

            auto build_root_name = std::string { graph.get_build_root_name() };
            if (node->type == pup::NodeType::Generated && !build_root_name.empty() && node_path.starts_with(build_root_name + "/")) {
                node_path = node_path.substr(build_root_name.size() + 1);
            }

            auto file_path = (node->type == pup::NodeType::Generated)
                ? output_root / node_path
                : source_root / node_path;

            auto content_hash = pup::Hash256 {};
            auto file_size = std::uint64_t { 0 };

            if (std::filesystem::exists(file_path)) {
                auto hash_result = pup::sha256_file(file_path);
                if (hash_result) {
                    content_hash = *hash_result;
                } else {
                    fprintf(stderr, "Warning: Failed to hash file: %s\n", file_path.c_str());
                }

                auto ec = std::error_code {};
                file_size = std::filesystem::file_size(file_path, ec);
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
                .content_hash = content_hash,
            };
            index.add_file(std::move(entry));
            path_to_id[node_path] = id;
        } else if (node->type == pup::NodeType::Directory || node->type == pup::NodeType::GeneratedDir) {
            auto node_path = graph.get_full_path(id);
            auto build_root_name = std::string { graph.get_build_root_name() };
            if (node->type == pup::NodeType::GeneratedDir && !build_root_name.empty() && node_path.starts_with(build_root_name + "/")) {
                node_path = node_path.substr(build_root_name.size() + 1);
            }

            auto node_name_sv = pup::graph::get_name(graph.graph(), id);
            auto entry_name = (id == pup::BUILD_ROOT_ID) ? std::string {} : std::string { node_name_sv };

            auto entry = pup::index::FileEntry {
                .id = id,
                .parent_id = node->parent_dir,
                .src_id = 0,
                .type = node->type,
                .flags = node->flags,
                .name = entry_name,
                .path = node_path,
                .size = 0,
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
auto serialize_command_nodes(
    pup::graph::BuildGraph const& graph,
    pup::index::Index& index
) -> void
{
    for (auto id : graph.all_nodes()) {
        if (!pup::is_command_id(id)) {
            continue;
        }
        auto const* cmd = graph.get_command_node(id);
        if (!cmd) {
            continue;
        }

        auto entry = pup::index::CommandEntry {
            .id = id,
            .dir_id = 0,
            .command = std::string { pup::graph::get_command_str(graph.graph(), id) },
            .display = std::string { pup::graph::get_display_str(graph.graph(), id) },
            .env = {},
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
        if (!pup::is_command_id(id) && id > max_file_id) {
            max_file_id = id;
        }
    }
    return pup::NodeId { max_file_id + 1 };
}

/// Process discovered implicit dependencies from compiler output.
/// Adds new file entries and implicit edges to the index.
auto process_implicit_deps(
    std::unordered_map<pup::NodeId, std::vector<std::string>> const& discovered_deps,
    pup::graph::BuildGraph const& graph,
    ImplicitDepContext& ctx
) -> void
{
    for (auto const& [cmd_id, deps] : discovered_deps) {
        for (auto const& dep_path : deps) {
            auto abs_path = resolve_path(dep_path, ctx.source_root);

            auto rel_path = std::string {};
            if (pup::is_path_under(abs_path, ctx.source_root)) {
                rel_path = std::filesystem::relative(abs_path, ctx.source_root).string();
            } else {
                rel_path = abs_path.string();
            }

            auto build_root_name = graph.get_build_root_name();
            auto build_prefix = std::string { build_root_name } + "/";
            if (!build_root_name.empty() && rel_path.starts_with(build_prefix)) {
                rel_path = rel_path.substr(build_prefix.size());
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
    auto commands_with_new_deps = std::set<pup::NodeId> {};
    for (auto const& [cmd_id, _] : discovered_deps) {
        commands_with_new_deps.insert(cmd_id);
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
        auto abs_path = resolve_path(old_file->path, ctx.source_root);
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

            auto cmd_node_id = graph.find_by_command(cmd->command);
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
    std::filesystem::path const& source_root,
    std::filesystem::path const& output_root,
    pup::index::Index const* old_index = nullptr
) -> pup::index::Index
{
    // Serialize file/directory nodes from the build graph
    auto [index, path_to_id] = serialize_graph_nodes(graph, source_root, output_root);

    // Serialize command nodes
    serialize_command_nodes(graph, index);

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
    process_implicit_deps(discovered_deps, graph, ctx);

    // Preserve implicit edges from the old index for commands that weren't rebuilt
    if (old_index) {
        preserve_old_implicit_edges(*old_index, discovered_deps, ctx);
    }

    // Compute Merkle hashes for directories (enables O(log n) change detection)
    index.compute_merkle_hashes();

    return index;
}

/// Build mode precedence (highest to lowest):
/// 1. Incremental - if old index exists and files changed
/// 2. Targets - if specific output targets requested (and not incremental)
/// 3. Subset - exclude config commands from full build
/// 4. Full - build everything
enum class BuildMode {
    Incremental,
    Targets,
    Subset,
    Full,
};

auto determine_build_mode(
    bool has_targets,
    bool use_incremental,
    bool has_config_cmds
) -> BuildMode
{
    if (use_incremental) {
        return BuildMode::Incremental;
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
        printf("[%.*s] Implicit dependency tracking enabled\n", static_cast<int>(variant_name.size()), variant_name.data());
    }

    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
        .keep_going = opts.keep_going,
        .auto_init = true,
        .root_config_only = false,
        .scanner_registry = scanner_ptr,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        fprintf(stderr, "[%.*s] Error: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), result.error().message.c_str());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;

    // Build requires tup.config (configure creates it)
    auto config_path = ctx.layout().output_root / "tup.config";
    if (!std::filesystem::exists(config_path)) {
        fprintf(stderr, "[%.*s] Error: No tup.config found. Run 'pup configure' first.\n", static_cast<int>(variant_name.size()), variant_name.data());
        return EXIT_FAILURE;
    }

    auto num_commands = std::size_t { ctx.graph().nodes_of_type(pup::NodeType::Command).size() };

    if (num_commands == 0) {
        printf("[%.*s] Nothing to do.\n", static_cast<int>(variant_name.size()), variant_name.data());
        return EXIT_SUCCESS;
    }

    // Validate output targets exist in graph and collect their NodeIds
    // Generated outputs are stored under BUILD_ROOT_ID, not SOURCE_ROOT_ID
    auto target_node_ids = std::vector<pup::NodeId> {};
    for (auto const& target : opts.output_targets) {
        auto node_id = ctx.graph().find_by_path(target, pup::BUILD_ROOT_ID);
        if (!node_id) {
            fprintf(stderr, "[%.*s] Error: %s is not in build graph\n", static_cast<int>(variant_name.size()), variant_name.data(), target.c_str());
            return EXIT_FAILURE;
        }
        auto const* node = ctx.graph().get_file_node(*node_id);
        if (!node || node->type != pup::NodeType::Generated) {
            fprintf(stderr, "[%.*s] Error: %s is not a build output\n", static_cast<int>(variant_name.size()), variant_name.data(), target.c_str());
            return EXIT_FAILURE;
        }
        target_node_ids.push_back(*node_id);
        if (opts.verbose) {
            printf("[%.*s] Output target: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), target.c_str());
        }
    }

    auto index_path = ctx.layout().index_path();
    auto const* old_idx_ptr = ctx.old_index();
    auto use_incremental = false;
    auto changed_files = std::vector<std::string> {};

    if (old_idx_ptr) {
        auto const& idx = *old_idx_ptr;

        if (opts.verbose && idx.has_merkle_hashes()) {
            printf("[%.*s] Index has Merkle hashes (v4 format)\n", static_cast<int>(variant_name.size()), variant_name.data());
        }

        auto scopes = compute_build_scopes(opts, ctx.layout());
        auto upstream_files = std::set<std::string> {};
        if (opts.include_all_deps && !scopes.empty()) {
            upstream_files = collect_upstream_files(ctx.graph(), scopes);
        }
        if (opts.verbose) {
            if (scopes.empty()) {
                printf("[%.*s] Full project build\n", static_cast<int>(variant_name.size()), variant_name.data());
            } else {
                printf("[%.*s] Scoped build:", static_cast<int>(variant_name.size()), variant_name.data());
                for (auto const& s : scopes) {
                    printf(" %s", s.c_str());
                }
                if (opts.include_all_deps) {
                    printf(" (+%zu upstream deps)", upstream_files.size());
                }
                printf("\n");
            }
        }

        changed_files = find_changed_files_with_implicit(ctx.layout().source_root, ctx.layout().output_root, idx, scopes, upstream_files, opts.verbose);
        changed_files = expand_implicit_deps(changed_files, idx, ctx.graph());

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

        // Detect new commands (in fresh graph but not in old index)
        for (auto id : ctx.graph().all_nodes()) {
            if (!pup::is_command_id(id)) {
                continue;
            }
            auto const* node = ctx.graph().get_command_node(id);
            if (!node) {
                continue;
            }

            auto cmd_sv = pup::graph::get_command_str(ctx.graph().graph(), id);
            if (!idx.find_command_by_command(std::string { cmd_sv })) {
                for (auto output_id : ctx.graph().get_outputs(id)) {
                    auto output_path = ctx.graph().get_full_path(output_id);
                    if (!output_path.empty()) {
                        changed_files.push_back(output_path);
                    }
                }
                if (opts.verbose) {
                    auto display_sv = pup::graph::get_display_str(ctx.graph().graph(), id);
                    printf("[%.*s]   New command: %.*s\n", static_cast<int>(variant_name.size()), variant_name.data(), static_cast<int>(display_sv.size()), display_sv.data());
                }
            }
        }

        // Detect removed commands (in old index but not in fresh graph)
        // and delete their stale outputs
        for (auto const& cmd : idx.commands()) {
            if (ctx.graph().find_by_command(cmd.command)) {
                continue;
            }

            for (auto const* edge : idx.edges_from(cmd.id)) {
                auto const* file = idx.find_file_by_id(edge->to);
                if (!file || file->type != pup::NodeType::Generated) {
                    continue;
                }

                // Paths are source-relative. Generated files exist at output_root.
                auto abs_path = ctx.layout().output_root / file->path;
                if (std::filesystem::exists(abs_path)) {
                    if (opts.dry_run) {
                        printf("[%.*s] Would remove stale: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), file->path.c_str());
                    } else {
                        auto ec = std::error_code {};
                        if (std::filesystem::remove(abs_path, ec)) {
                            if (opts.verbose) {
                                printf("[%.*s]   Removed stale: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), file->path.c_str());
                            }
                        }
                    }
                }
            }

            if (opts.verbose) {
                printf("[%.*s]   Removed command: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), cmd.display.c_str());
            }
        }

        if (changed_files.empty()) {
            printf("[%.*s] Nothing to do (up to date).\n", static_cast<int>(variant_name.size()), variant_name.data());
            if (opts.stat) {
                print_stats(idx, num_commands, 0);
            }
            return EXIT_SUCCESS;
        }

        use_incremental = true;
        if (opts.verbose) {
            printf("[%.*s] Incremental build: %zu changed files\n", static_cast<int>(variant_name.size()), variant_name.data(), changed_files.size());
        }
    }

    auto sched_opts = pup::exec::SchedulerOptions {
        .jobs = opts.jobs,
        .keep_going = opts.keep_going,
        .dry_run = opts.dry_run,
        .verbose = opts.verbose,
        .source_root = ctx.layout().source_root,
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
            printf("[%.*s] %s\n", static_cast<int>(variant_name.size()), variant_name.data(), job.display.c_str());
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
            fprintf(stderr, "[%.*s] FAILED: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), job.display.c_str());
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
                auto ec = std::error_code {};
                auto resolved = std::filesystem::path {};
                if (std::filesystem::path { dep_path }.is_absolute()) {
                    resolved = std::filesystem::weakly_canonical(dep_path, ec);
                } else {
                    resolved = std::filesystem::weakly_canonical(job.working_dir / dep_path, ec);
                }
                if (ec) {
                    if (opts.verbose) {
                        fprintf(stderr, "Warning: Skipping dependency '%s': %s\n", dep_path.c_str(), ec.message().c_str());
                    }
                    continue;
                }

                if (pup::is_path_under(resolved, ctx.layout().source_root)) {
                    auto rel = std::filesystem::relative(resolved, ctx.layout().source_root, ec);
                    if (ec) {
                        if (opts.verbose) {
                            fprintf(stderr, "Warning: Cannot relativize '%s': %s\n", resolved.string().c_str(), ec.message().c_str());
                        }
                        continue;
                    }
                    deps.push_back(rel.string());
                } else {
                    deps.push_back(resolved.string());
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
    auto config_cmd_ids = std::set<NodeId> {};
    for (auto const& cfg : find_config_commands(ctx.graph(), ctx.layout().source_root)) {
        config_cmd_ids.insert(cfg.cmd_id);
    }

    auto start = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
    auto build_result = pup::Result<pup::exec::BuildStats> {};

    auto mode = determine_build_mode(
        !target_node_ids.empty(),
        use_incremental,
        !config_cmd_ids.empty()
    );

    switch (mode) {
    case BuildMode::Incremental:
        build_result = scheduler.build_incremental(ctx.graph(), changed_files);
        break;
    case BuildMode::Targets:
        build_result = scheduler.build_targets(ctx.graph(), target_node_ids);
        break;
    case BuildMode::Subset: {
        auto non_config_cmds = std::set<NodeId> {};
        for (auto id : ctx.graph().all_nodes()) {
            if (is_command_id(id) && !config_cmd_ids.contains(id)) {
                non_config_cmds.insert(id);
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
        fprintf(stderr, "[%.*s] Build failed: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), build_result.error().message.c_str());
        return EXIT_FAILURE;
    }

    auto const& stats = *build_result;
    if (stats.failed_jobs > 0) {
        printf("[%.*s] Build completed: %zu commands (%zu failed) in %ldms\n", static_cast<int>(variant_name.size()), variant_name.data(), stats.completed_jobs, stats.failed_jobs, static_cast<long>(duration.count()));
    } else {
        printf("[%.*s] Build completed: %zu commands in %ldms\n", static_cast<int>(variant_name.size()), variant_name.data(), stats.completed_jobs, static_cast<long>(duration.count()));
    }

    auto final_index = std::optional<pup::index::Index> {};
    if (!opts.dry_run) {
        // Save index even after partial failures - successful outputs are recorded
        // so they won't be rebuilt. Failed outputs don't exist, so stat will fail
        // and they'll be detected as changed on next build.
        auto index = pup::index::Index { build_index(ctx.graph(), discovered_deps, ctx.layout().source_root, ctx.layout().output_root, old_idx_ptr) };

        auto index_save_start = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
        auto write_result = pup::Result<void> { pup::index::write_index(index_path, index) };
        auto index_save_end = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
        pup::thread_metrics().index_save_time = std::chrono::duration_cast<std::chrono::milliseconds>(index_save_end - index_save_start);

        if (!write_result) {
            fprintf(stderr, "[%.*s] Warning: Failed to save index: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), write_result.error().message.c_str());
        } else if (opts.verbose) {
            printf("[%.*s] Saved index: %zu files, %zu commands, %zu edges\n", static_cast<int>(variant_name.size()), variant_name.data(), index.file_count(), index.command_count(), index.edge_count());
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
