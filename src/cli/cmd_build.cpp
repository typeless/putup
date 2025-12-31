// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/multi_variant.hpp"
#include "pup/core/hash.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/metrics.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/core/types.hpp"
#include "pup/core/y_combinator.hpp"
#include "pup/exec/scheduler.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/rule_pattern.hpp"
#include "pup/index/entry.hpp"
#include "pup/index/writer.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>
#include <unordered_map>

#include <fmt/core.h>

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

    fmt::print("\nStats:\n");
    fmt::print("  Tupfiles parsed:    {:>6}\n", metrics.tupfiles_parsed);
    fmt::print("  Commands:           {:>6} total, {} executed\n", num_commands, commands_executed);
    fmt::print("  Files checked:      {:>6} ({} changed)\n", metrics.files_checked, metrics.files_changed);
    fmt::print("  Files in index:     {:>6}\n", index.file_count());
    fmt::print("  Edges in graph:     {:>6}\n", index.edge_count());
    fmt::print("  Implicit deps:      {:>6}\n", implicit_deps_count);
    fmt::print("  Hash computations:  {:>6}\n", metrics.hash_computations);
    fmt::print("  Stat calls:         {:>6}\n", metrics.stat_calls);
    if (metrics.index_load_time.count() > 0 || metrics.index_save_time.count() > 0) {
        fmt::print("  Index I/O:          {:>6}ms load, {}ms save\n", metrics.index_load_time.count(), metrics.index_save_time.count());
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
    std::function<void(pup::NodeId)> collect = [&](pup::NodeId id) {
        if (!visited.insert(id).second) {
            return;
        }

        auto const* node = graph.get_node(id);
        if (!node) {
            return;
        }

        if (node->type == pup::NodeType::File || node->type == pup::NodeType::Generated) {
            auto path = graph.get_full_path(id);
            if (!path.empty()) {
                upstream.insert(path);
            }
        }

        // Walk to inputs (upstream)
        for (auto input_id : graph.get_inputs(id)) {
            collect(input_id);
        }

        // Also follow order-only deps
        for (auto dep_id : graph.get_order_only(id)) {
            collect(dep_id);
        }
    };

    // Find commands in scope and collect their upstream deps
    for (auto id : graph.all_nodes()) {
        auto const* node = graph.get_node(id);
        if (!node || node->type != pup::NodeType::Command) {
            continue;
        }

        // Check if command's source_dir is in any scope
        if (!pup::is_path_in_any_scope(node->source_dir, scopes)) {
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
                fmt::print("  Changed (stat failed): {}\n", file.path);
            }
            ++metrics.files_changed;
            changed.push_back(file.path);
            continue;
        }

        // Size check (fast path)
        auto current_size = stat_result->size;
        if (current_size != file.size) {
            if (verbose) {
                fmt::print("  Changed (size): {}\n", file.path);
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
                    fmt::print("  Changed (hash): {}\n", file.path);
                }
                ++metrics.files_changed;
                changed.push_back(file.path);
            }
        }
    }

    return changed;
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

    for (auto const& path : changed) {
        auto it = path_to_file.find(path);
        if (it == path_to_file.end()) {
            continue;
        }

        auto file_id = pup::NodeId { it->second->id };

        for (auto const& edge : index.edges()) {
            if (edge.from != file_id) {
                continue;
            }

            if (edge.type == pup::LinkType::Implicit) {
                auto cmd_id = pup::NodeId { edge.to };
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

            if (edge.type == pup::LinkType::Sticky) {
                auto cmd_id = pup::NodeId { edge.to };
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
    }

    return result;
}

auto build_index(
    pup::graph::BuildGraph const& graph,
    std::unordered_map<pup::NodeId, std::vector<std::string>> const& discovered_deps,
    std::filesystem::path const& source_root,
    std::filesystem::path const& output_root,
    pup::index::Index const* old_index = nullptr
) -> pup::index::Index
{
    auto index = pup::index::Index {};
    auto path_to_id = std::unordered_map<std::string, pup::NodeId> {};

    auto max_file_id = pup::NodeId { 0 };
    for (auto id : graph.all_nodes()) {
        auto const* node = graph.get_node(id);
        if (!node) {
            continue;
        }
        if (!pup::is_command_id(id) && id > max_file_id) {
            max_file_id = id;
        }

        // Skip ghost nodes - they're transient placeholders that should have been
        // upgraded to Generated or caught as errors during build
        if (node->type == pup::NodeType::Ghost) {
            continue;
        }

        if (node->type == pup::NodeType::File || node->type == pup::NodeType::Generated) {
            auto node_path = graph.get_full_path(id);
            if (node_path.empty()) {
                continue;
            }

            // For Generated nodes under BUILD_ROOT_ID, get_full_path returns
            // the path WITH the build root name (e.g., "build/src/main.o").
            // Strip it to get the source-relative path (e.g., "src/main.o").
            auto build_root_name = std::string { graph.get_build_root_name() };
            if (node->type == pup::NodeType::Generated && !build_root_name.empty() && node_path.starts_with(build_root_name + "/")) {
                node_path = node_path.substr(build_root_name.size() + 1);
            }

            // Paths are source-relative. Generated files exist at output_root,
            // source files exist at source_root.
            auto file_path = (node->type == pup::NodeType::Generated)
                ? output_root / node_path
                : source_root / node_path;

            auto content_hash = pup::Hash256 {};
            auto file_size = std::uint64_t { 0 };

            if (std::filesystem::exists(file_path)) {
                auto hash_result = pup::sha256_file(file_path);
                if (hash_result) {
                    content_hash = *hash_result;
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
                .name = node->name,
                .path = node_path,
                .size = file_size,
                .content_hash = content_hash,
            };
            index.add_file(std::move(entry));
            path_to_id[node_path] = id;
        } else if (node->type == pup::NodeType::Directory || node->type == pup::NodeType::GeneratedDir) {
            auto node_path = graph.get_full_path(id);
            // For GeneratedDir nodes under BUILD_ROOT_ID, strip the build root name
            auto build_root_name = std::string { graph.get_build_root_name() };
            if (node->type == pup::NodeType::GeneratedDir && !build_root_name.empty() && node_path.starts_with(build_root_name + "/")) {
                node_path = node_path.substr(build_root_name.size() + 1);
            }

            // For BUILD_ROOT_ID itself, store empty name so it doesn't contribute
            // to path reconstruction during index loading. This is essential for
            // variant builds where Generated files should have source-relative paths.
            auto entry_name = (id == pup::BUILD_ROOT_ID) ? std::string {} : node->name;

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
        } else if (node->type == pup::NodeType::Variable) {
            // Variable nodes must be in index to maintain consecutive ID sequence
            auto entry = pup::index::FileEntry {
                .id = id,
                .parent_id = node->parent_dir,
                .src_id = 0,
                .type = node->type,
                .flags = node->flags,
                .name = node->name,
                .path = {},
                .size = 0,
                .content_hash = node->content_hash,
            };
            index.add_file(std::move(entry));
        } else if (node->type == pup::NodeType::Group) {
            // Group nodes must be in index to maintain consecutive ID sequence
            auto entry = pup::index::FileEntry {
                .id = id,
                .parent_id = node->parent_dir,
                .src_id = 0,
                .type = node->type,
                .flags = node->flags,
                .name = node->name,
                .path = {},
                .size = 0,
                .content_hash = {},
            };
            index.add_file(std::move(entry));
        } else if (node->type == pup::NodeType::Command) {
            auto entry = pup::index::CommandEntry {
                .id = id,
                .dir_id = node->parent_dir,
                .command = node->command,
                .display = node->display,
                .env = {},
            };
            index.add_command(std::move(entry));
        }
    }

    for (auto const& edge : graph.edges()) {
        index.add_edge(pup::index::EdgeEntry {
            .from = edge.from,
            .to = edge.to,
            .type = edge.type,
            .group_cmd_id = edge.group_cmd_id,
        });
    }

    auto next_id = pup::NodeId { max_file_id + 1 };
    auto added_edges = std::set<std::pair<pup::NodeId, pup::NodeId>> {};

    auto get_or_create_dir = pup::YCombinator { [&](
                                                    auto const& self,
                                                    std::filesystem::path const& dir_path
                                                ) -> pup::NodeId {
        auto normalized = dir_path.lexically_normal();
        auto path_str = normalized.string();

        if (path_str.empty() || path_str == ".") {
            return pup::NodeId { 0 };
        }

        if (auto it = path_to_id.find(path_str); it != path_to_id.end()) {
            return it->second;
        }

        if (path_str == "/") {
            auto dir_id = next_id++;
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
            index.add_file(std::move(entry));
            path_to_id["/"] = dir_id;
            return dir_id;
        }

        auto parent_path = normalized.parent_path();
        auto parent_id = self(parent_path);
        auto basename = normalized.filename().string();

        auto dir_id = next_id++;
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
        index.add_file(std::move(entry));
        path_to_id[path_str] = dir_id;
        return dir_id;
    } };

    auto create_implicit_file = [&](
                                    std::filesystem::path const& abs_path,
                                    std::string const& rel_path
                                ) -> pup::NodeId {
        auto content_hash = pup::Hash256 {};
        auto file_size = std::uint64_t { 0 };
        if (std::filesystem::exists(abs_path)) {
            auto hash_result = pup::sha256_file(abs_path);
            if (hash_result) {
                content_hash = *hash_result;
            }

            auto ec = std::error_code {};
            file_size = std::filesystem::file_size(abs_path, ec);
        }

        // Create parent directories first - they get assigned IDs before the file
        auto fs_path = std::filesystem::path { rel_path };
        auto parent_id = get_or_create_dir(fs_path.parent_path());
        auto basename = fs_path.filename().string();

        // Now assign file ID (after directory IDs to maintain consecutive ordering)
        auto file_id = next_id++;

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
        index.add_file(std::move(entry));
        path_to_id[rel_path] = file_id;
        return file_id;
    };

    for (auto const& [cmd_id, deps] : discovered_deps) {
        for (auto const& dep_path : deps) {
            // Implicit deps (from -MD) are source headers
            auto abs_path = resolve_path(dep_path, source_root);

            auto rel_path = std::string {};
            if (pup::is_path_under(abs_path, source_root)) {
                rel_path = std::filesystem::relative(abs_path, source_root).string();
            } else {
                rel_path = abs_path.string();
            }

            // For variant builds, implicit deps from the build directory (e.g., "build/include/header.h")
            // need to be normalized to source-relative paths (e.g., "include/header.h") to match
            // how Generated nodes are stored in the index.
            auto build_root_name = graph.get_build_root_name();
            auto build_prefix = std::string { build_root_name } + "/";
            if (!build_root_name.empty() && rel_path.starts_with(build_prefix)) {
                rel_path = rel_path.substr(build_prefix.size());
            }

            auto it = path_to_id.find(rel_path);
            auto dep_id = it != path_to_id.end() ? it->second : create_implicit_file(abs_path, rel_path);

            auto edge_key = std::pair { dep_id, cmd_id };
            if (added_edges.insert(edge_key).second) {
                index.add_edge(pup::index::EdgeEntry {
                    .from = dep_id,
                    .to = cmd_id,
                    .type = pup::LinkType::Implicit,
                    .group_cmd_id = 0,
                });
            }
        }
    }

    if (old_index) {
        auto commands_with_new_deps = std::set<pup::NodeId> {};
        for (auto const& [cmd_id, _] : discovered_deps) {
            commands_with_new_deps.insert(cmd_id);
        }

        auto old_path_to_new_id = std::unordered_map<std::string, pup::NodeId> {};
        for (auto const& file : old_index->files()) {
            auto it = path_to_id.find(file.path);
            if (it != path_to_id.end()) {
                old_path_to_new_id[file.path] = it->second;
            }
        }

        for (auto const& edge : old_index->edges()) {
            if (edge.type != pup::LinkType::Implicit) {
                continue;
            }

            if (commands_with_new_deps.contains(edge.to)) {
                continue;
            }

            auto const* old_file = old_index->find_file_by_id(edge.from);
            if (!old_file) {
                continue;
            }

            auto new_file_it = path_to_id.find(old_file->path);
            // Old implicit deps are source headers
            auto abs_path = resolve_path(old_file->path, source_root);
            auto new_from_id = new_file_it != path_to_id.end()
                ? new_file_it->second
                : create_implicit_file(abs_path, old_file->path);

            auto edge_key = std::pair { new_from_id, edge.to };
            if (added_edges.insert(edge_key).second) {
                index.add_edge(pup::index::EdgeEntry {
                    .from = new_from_id,
                    .to = edge.to,
                    .type = pup::LinkType::Implicit,
                    .group_cmd_id = 0,
                });
            }
        }
    }

    // Compute Merkle hashes for directories (enables O(log n) change detection)
    index.compute_merkle_hashes();

    return index;
}

/// Build a single variant with the given options.
/// Expects opts.build_dirs to contain at most one element.
auto build_single_variant(
    Options const& opts,
    std::string_view variant_name
) -> int
{
    // Helper to format output with variant prefix
    auto vprint = [&](std::string_view fmt_str, auto&&... args) {
        fmt::print("[{}] ", variant_name);
        fmt::print(fmt::runtime(fmt_str), std::forward<decltype(args)>(args)...);
    };

    auto scanner_registry = make_scanner_registry();
    auto* const scanner_ptr = scanner_registry ? &*scanner_registry : nullptr;
    if (scanner_ptr && opts.verbose) {
        vprint("Implicit dependency tracking enabled\n");
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
        fmt::print(stderr, "[{}] Error: {}\n", variant_name, result.error().message);
        return EXIT_FAILURE;
    }

    auto& ctx = *result;

    // Build requires tup.config (configure creates it)
    auto config_path = ctx.layout().output_root / "tup.config";
    if (!std::filesystem::exists(config_path)) {
        fmt::print(stderr, "[{}] Error: No tup.config found. Run 'pup configure' first.\n", variant_name);
        return EXIT_FAILURE;
    }

    auto num_commands = std::size_t { ctx.graph().nodes_of_type(pup::NodeType::Command).size() };

    if (num_commands == 0) {
        vprint("Nothing to do.\n");
        return EXIT_SUCCESS;
    }

    // Validate output targets exist in graph
    // Generated outputs are stored under BUILD_ROOT_ID, not SOURCE_ROOT_ID
    for (auto const& target : opts.output_targets) {
        auto node_id = ctx.graph().find_by_path(target, pup::BUILD_ROOT_ID);
        if (!node_id) {
            fmt::print(stderr, "[{}] Error: {} is not in build graph\n", variant_name, target);
            return EXIT_FAILURE;
        }
        auto const* node = ctx.graph().get_node(*node_id);
        if (!node || node->type != pup::NodeType::Generated) {
            fmt::print(stderr, "[{}] Error: {} is not a build output\n", variant_name, target);
            return EXIT_FAILURE;
        }
        if (opts.verbose) {
            vprint("Output target: {}\n", target);
        }
    }

    auto index_path = ctx.layout().index_path();
    auto const* old_idx_ptr = ctx.old_index();
    auto use_incremental = false;
    auto changed_files = std::vector<std::string> {};

    if (old_idx_ptr) {
        auto const& idx = *old_idx_ptr;

        if (opts.verbose && idx.has_merkle_hashes()) {
            vprint("Index has Merkle hashes (v4 format)\n");
        }

        auto scopes = compute_build_scopes(opts, ctx.layout());
        auto upstream_files = std::set<std::string> {};
        if (opts.include_all_deps && !scopes.empty()) {
            upstream_files = collect_upstream_files(ctx.graph(), scopes);
        }
        if (opts.verbose) {
            if (scopes.empty()) {
                vprint("Full project build\n");
            } else {
                fmt::print("[{}] Scoped build:", variant_name);
                for (auto const& s : scopes) {
                    fmt::print(" {}", s);
                }
                if (opts.include_all_deps) {
                    fmt::print(" (+{} upstream deps)", upstream_files.size());
                }
                fmt::print("\n");
            }
        }

        changed_files = find_changed_files_with_implicit(ctx.layout().source_root, ctx.layout().output_root, idx, scopes, upstream_files, opts.verbose);
        changed_files = expand_implicit_deps(changed_files, idx, ctx.graph());

        // Add output targets to force their rebuild
        // Output targets are source-relative (e.g., "hello"), but changed_files uses
        // full paths from get_full_path() which include build root prefix (e.g., "build-debug/hello").
        auto build_root_name = std::string { ctx.graph().get_build_root_name() };
        for (auto const& output_path : opts.output_targets) {
            auto prefixed = build_root_name.empty() ? output_path : (build_root_name + "/" + output_path);
            if (std::ranges::find(changed_files, prefixed) == changed_files.end()) {
                changed_files.push_back(prefixed);
            }
        }

        // Detect new commands (in fresh graph but not in old index)
        for (auto id : ctx.graph().all_nodes()) {
            auto const* node = ctx.graph().get_node(id);
            if (!node || node->type != pup::NodeType::Command) {
                continue;
            }

            if (!idx.find_command_by_command(node->command)) {
                for (auto output_id : ctx.graph().get_outputs(id)) {
                    auto output_path = ctx.graph().get_full_path(output_id);
                    if (!output_path.empty()) {
                        changed_files.push_back(output_path);
                    }
                }
                if (opts.verbose) {
                    vprint("  New command: {}\n", node->display);
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
                        vprint("Would remove stale: {}\n", file->path);
                    } else {
                        auto ec = std::error_code {};
                        if (std::filesystem::remove(abs_path, ec)) {
                            if (opts.verbose) {
                                vprint("  Removed stale: {}\n", file->path);
                            }
                        }
                    }
                }
            }

            if (opts.verbose) {
                vprint("  Removed command: {}\n", cmd.display);
            }
        }

        if (changed_files.empty()) {
            vprint("Nothing to do (up to date).\n");
            if (opts.stat) {
                print_stats(idx, num_commands, 0);
            }
            return EXIT_SUCCESS;
        }

        use_incremental = true;
        if (opts.verbose) {
            vprint("Incremental build: {} changed files\n", changed_files.size());
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

    scheduler.on_job_start([&](pup::exec::BuildJob const& job) {
        if (opts.verbose || opts.dry_run) {
            fmt::print("[{}] {}\n", variant_name, job.display);
        }
    });

    scheduler.on_job_complete([&](pup::exec::BuildJob const& job, pup::exec::JobResult const& job_result) {
        if (!job_result.success) {
            fmt::print(stderr, "[{}] FAILED: {}\n", variant_name, job.display);
            if (!job_result.output.empty()) {
                fmt::print(stderr, "[{}] {}\n", variant_name, job_result.output);
            }
        } else if (!job_result.discovered_deps.empty()) {
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
                        fmt::print(stderr, "Warning: Skipping dependency '{}': {}\n", dep_path, ec.message());
                    }
                    continue;
                }

                if (pup::is_path_under(resolved, ctx.layout().source_root)) {
                    auto rel = std::filesystem::relative(resolved, ctx.layout().source_root, ec);
                    if (ec) {
                        if (opts.verbose) {
                            fmt::print(stderr, "Warning: Cannot relativize '{}': {}\n", resolved.string(), ec.message());
                        }
                        continue;
                    }
                    deps.push_back(rel.string());
                } else {
                    deps.push_back(resolved.string());
                }
            }
        }
    });

    auto completed = std::size_t { 0 };
    scheduler.on_progress([&](std::size_t done, std::size_t total) {
        completed = done;
        if (!opts.verbose) {
            fmt::print("\r[{}] [{}/{}] ", variant_name, done, total);
            std::fflush(stdout);
        }
    });

    auto start = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
    auto build_result = pup::Result<pup::exec::BuildStats> {};
    if (!opts.output_targets.empty() && !use_incremental) {
        // Single output targets without old_index - use incremental with just target paths
        // Output targets are source-relative (e.g., "hello"), but get_full_path returns
        // paths with build root prefix (e.g., "build/hello"). Add the prefix.
        auto build_root_name = std::string { ctx.graph().get_build_root_name() };
        auto prefixed_targets = std::vector<std::string> {};
        for (auto const& target : opts.output_targets) {
            if (!build_root_name.empty()) {
                prefixed_targets.push_back(build_root_name + "/" + target);
            } else {
                prefixed_targets.push_back(target);
            }
        }
        auto empty_index = pup::index::Index {};
        build_result = scheduler.build_incremental(ctx.graph(), empty_index, prefixed_targets);
    } else if (use_incremental && old_idx_ptr) {
        build_result = scheduler.build_incremental(ctx.graph(), *old_idx_ptr, changed_files);
    } else {
        build_result = scheduler.build(ctx.graph());
    }
    auto end = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
    auto duration = std::chrono::milliseconds { std::chrono::duration_cast<std::chrono::milliseconds>(end - start) };

    if (!opts.verbose) {
        fmt::print("\n");
    }

    if (!build_result) {
        fmt::print(stderr, "[{}] Build failed: {}\n", variant_name, build_result.error().message);
        return EXIT_FAILURE;
    }

    auto const& stats = *build_result;
    if (stats.failed_jobs > 0) {
        vprint("Build completed: {} commands ({} failed) in {}ms\n", stats.completed_jobs, stats.failed_jobs, duration.count());
    } else {
        vprint("Build completed: {} commands in {}ms\n", stats.completed_jobs, duration.count());
    }

    auto final_index = std::optional<pup::index::Index> {};
    if (!opts.dry_run) {
        // Save index even after partial failures - successful outputs are recorded
        // so they won't be rebuilt. Failed outputs don't exist, so stat will fail
        // and they'll be detected as changed on next build.
        auto index = pup::index::Index { build_index(ctx.graph(), discovered_deps, ctx.layout().source_root, ctx.layout().output_root, old_idx_ptr) };
        auto writer = pup::index::IndexWriter {};

        auto index_save_start = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
        auto write_result = pup::Result<void> { writer.write(index_path, index) };
        auto index_save_end = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
        pup::thread_metrics().index_save_time = std::chrono::duration_cast<std::chrono::milliseconds>(index_save_end - index_save_start);

        if (!write_result) {
            fmt::print(stderr, "[{}] Warning: Failed to save index: {}\n", variant_name, write_result.error().message);
        } else if (opts.verbose) {
            vprint("Saved index: {} files, {} commands, {} edges\n", index.file_count(), index.command_count(), index.edge_count());
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
