// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/config_commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/index_serialize.hpp"
#include "pup/cli/multi_variant.hpp"
#include "pup/cli/options.hpp"
#include "pup/core/clock.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/hash.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/metrics.hpp"
#include "pup/core/node_id_map.hpp"
#include "pup/core/node_pair_set.hpp"
#include "pup/core/path.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/core/print.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/terminal.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/exec/progress_display.hpp"
#include "pup/exec/scheduler.hpp"
#include "pup/graph/dag.hpp"
#include "pup/index/entry.hpp"
#include "pup/index/writer.hpp"
#include "pup/platform/file_io.hpp"
#include "pup/platform/path.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

namespace pup::cli {

namespace {
using DiscoveredDeps = Vec<std::pair<pup::NodeId, Vec<StringId>>>;

auto path_id_find(PathIdMap const& m, std::string_view key) -> std::pair<StringId, pup::NodeId> const*
{
    auto& pool = pup::global_pool();
    auto pos = std::lower_bound(m.begin(), m.end(), key, [&pool](auto const& p, std::string_view k) { return pool.get(p.first) < k; });
    return (pos != m.end() && pool.get(pos->first) == key) ? pos : nullptr;
}

auto path_id_insert(PathIdMap& m, StringId key, pup::NodeId id) -> void
{
    auto& pool = pup::global_pool();
    auto key_sv = pool.get(key);
    auto pos = std::lower_bound(m.begin(), m.end(), key_sv, [&pool](auto const& p, std::string_view k) { return pool.get(p.first) < k; });
    if (pos != m.end() && pool.get(pos->first) == key_sv) {
        pos->second = id;
    } else {
        m.insert(pos, std::pair<StringId, pup::NodeId> { key, id });
    }
}

auto discovered_deps_get(DiscoveredDeps& m, pup::NodeId key) -> Vec<StringId>&
{
    for (auto& [k, v] : m) {
        if (k == key) {
            return v;
        }
    }
    m.emplace_back(key, Vec<StringId> {});
    return m.back().second;
}

auto print_stats(
    pup::index::Index const& index,
    std::size_t num_commands,
    std::size_t commands_executed,
    pup::SteadyClock::time_point variant_start
) -> void
{
    auto metrics = pup::collect_metrics();
    metrics.total_time = std::chrono::duration_cast<std::chrono::microseconds>(pup::SteadyClock::now() - variant_start);

    auto implicit_deps_count = std::size_t { 0 };
    for (auto const& edge : index.edges()) {
        if (edge.type == pup::LinkType::Implicit) {
            ++implicit_deps_count;
        }
    }

    print("\nStats:\n");
    print("  Tupfiles parsed:    {}\n", Pad { metrics.tupfiles_parsed, 6 });
    print("  Commands:           {} total, {} executed\n", Pad { num_commands, 6 }, commands_executed);
    print("  Files checked:      {} ({} changed)\n", Pad { metrics.files_checked, 6 }, metrics.files_changed);
    print("  Files in index:     {}\n", Pad { index.file_count(), 6 });
    print("  Edges in graph:     {}\n", Pad { index.edge_count(), 6 });
    print("  Implicit deps:      {}\n", Pad { implicit_deps_count, 6 });
    print("  Hash computations:  {}\n", Pad { metrics.hash_computations, 6 });
    print("  Hashes skipped:     {} (stat cache)\n", Pad { metrics.hashes_skipped, 6 });
    print("  Stat calls:         {}\n", Pad { metrics.stat_calls, 6 });
    print("  String pool:        {} strings, {} bytes\n", Pad { metrics.pool_strings, 6 }, metrics.pool_bytes);

    auto as_ms = [](std::chrono::microseconds us) { return Fixed { static_cast<double>(us.count()) / 1000.0, 1, 8 }; };

    auto accounted = metrics.parse_time
        + metrics.index_load_time
        + metrics.command_index_time
        + metrics.change_detection_time
        + metrics.implicit_deps_time
        + metrics.new_commands_time
        + metrics.stale_outputs_time
        + metrics.job_list_time
        + metrics.exec_time
        + metrics.index_rebuild_time
        + metrics.index_save_time;

    print("\n  Phase timing:\n");
    print("    Parse:            {}ms ({} Tupfiles)\n", as_ms(metrics.parse_time), metrics.tupfiles_parsed);
    print("    Index load:       {}ms\n", as_ms(metrics.index_load_time));
    print("    Command index:    {}ms ({} expansions)\n", as_ms(metrics.command_index_time), metrics.command_expansions);
    print("    Change detection: {}ms ({} stats, {} hashes, {} skipped)\n", as_ms(metrics.change_detection_time), metrics.stat_calls, metrics.hash_computations, metrics.hashes_skipped);
    print("    Implicit deps:    {}ms\n", as_ms(metrics.implicit_deps_time));
    print("    New commands:     {}ms\n", as_ms(metrics.new_commands_time));
    print("    Stale outputs:    {}ms\n", as_ms(metrics.stale_outputs_time));
    print("    Job list:         {}ms\n", as_ms(metrics.job_list_time));
    print("    Command execution:{}ms\n", as_ms(metrics.exec_time));
    print("    Index rebuild:    {}ms\n", as_ms(metrics.index_rebuild_time));
    print("    Index save:       {}ms\n", as_ms(metrics.index_save_time));
    print("    Unaccounted:      {}ms\n", as_ms(metrics.total_time - accounted));
    print("  Total:              {}ms\n", as_ms(metrics.total_time));
}

auto strip_build_root_prefix(std::string_view path, std::string_view build_root_name) -> std::string_view
{
    if (!build_root_name.empty()) {
        auto prefix_len = build_root_name.size() + 1;
        if (path.size() > build_root_name.size() && path.starts_with(build_root_name) && path[build_root_name.size()] == '/') {
            return path.substr(prefix_len);
        }
    }
    return path;
}

template<typename... Args>
auto vprint(std::string_view variant_name, std::string_view pattern, Args const&... args) -> void
{
    print("[{}] ", variant_name);
    print(pattern, args...);
}

template<typename... Args>
auto veprint(std::string_view variant_name, std::string_view pattern, Args const&... args) -> void
{
    eprint("[{}] ", variant_name);
    eprint(pattern, args...);
}

auto is_tupfile(std::string_view path) -> bool
{
    return path.ends_with("/Tupfile") || path.ends_with("/Tuprules.tup")
        || path == "Tupfile" || path == "Tuprules.tup"
        || path.ends_with("/tup.config") || path == "tup.config";
}

/// Collect file paths that are implicit dependencies of in-scope commands.
/// These files (typically headers from .d files) must not be skipped by the
/// scope filter, even if they live outside the scoped directories.
auto collect_implicit_dep_files(
    pup::index::Index const& index,
    pup::Vec<pup::StringId> const& scopes
) -> Vec<StringId>
{
    auto& pool = pup::global_pool();
    auto result = Vec<StringId> {};

    // Find commands whose directory is in scope
    auto in_scope_cmds = pup::NodeIdMap32 {};
    for (auto const& cmd : index.commands()) {
        auto const* dir_file = index.find_file_by_id(cmd.dir_id);
        if (!dir_file) {
            continue;
        }
        auto dir_path = pool.get(dir_file->path);
        if (pup::is_path_in_any_scope(dir_path, scopes)) {
            in_scope_cmds.set(cmd.id, 1);
        }
    }

    // Collect files with implicit/sticky edges to in-scope commands
    for (auto const& edge : index.edges()) {
        if (edge.type != pup::LinkType::Implicit && edge.type != pup::LinkType::Sticky) {
            continue;
        }
        if (!in_scope_cmds.contains(edge.to)) {
            continue;
        }
        auto const* file = index.find_file_by_id(edge.from);
        if (file && !pup::is_empty(file->path)) {
            result.push_back(file->path);
        }
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

/// Collect paths declared only by commands whose conditional guard is unsatisfied.
/// Both branches of a config-driven conditional are in the graph; only the satisfied
/// one produces files, so the other branch's outputs will never exist on disk. A path
/// declared by both branches shares one node and has an active producer, so the test
/// is "no active producer" rather than "some inactive declarer".
auto collect_inactive_output_paths(pup::graph::BuildGraph const& state) -> Vec<StringId>
{
    auto const& g = state.graph;
    auto produced_by_active = pup::NodeIdMap32 {};
    auto declared_by_inactive = pup::NodeIdMap32 {};

    for (auto id : pup::graph::all_nodes(g)) {
        if (!pup::node_id::is_command(id)) {
            continue;
        }
        auto& outputs = pup::graph::is_guard_satisfied(g, id) ? produced_by_active : declared_by_inactive;
        for (auto output_id : pup::graph::get_outputs(g, id)) {
            outputs.set(output_id, 1);
        }
    }

    auto result = Vec<StringId> {};
    for (auto id : pup::graph::all_nodes(g)) {
        if (!declared_by_inactive.contains(id) || produced_by_active.contains(id)) {
            continue;
        }
        auto node_path = pup::graph::get_full_path(g, id, state.path_cache);
        if (!node_path.empty()) {
            result.push_back(pup::global_pool().intern(node_path));
        }
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

auto find_changed_files_with_implicit(
    std::string_view source_root,
    std::string_view config_root,
    pup::index::Index const& old_index,
    pup::Vec<pup::StringId> const& scopes,
    Vec<std::string_view> const& upstream_files,
    Vec<StringId> const& implicit_dep_files,
    Vec<StringId> const& inactive_outputs,
    bool verbose = false,
    bool no_stat_cache = false
) -> pup::Vec<StringId>
{
    auto changed = pup::Vec<StringId> {};
    auto& metrics = pup::thread_metrics();

    // Racy-clean threshold: files modified within 1 second of index save
    auto const save_time_ns = old_index.save_time_ns();
    auto constexpr RACY_CLEAN_THRESHOLD_NS = std::int64_t { 1'000'000'000 };

    for (auto const& file : old_index.files()) {
        auto const is_tracked_ghost = file.type == pup::NodeType::Ghost
            && file.content_hash != pup::ZERO_HASH && !pup::is_empty(file.path);
        if (file.type != pup::NodeType::File && file.type != pup::NodeType::Generated
            && !is_tracked_ghost) {
            continue;
        }

        // No active rule produces this, so its absence is not a change.
        if (std::binary_search(inactive_outputs.begin(), inactive_outputs.end(), file.path)) {
            continue;
        }

        auto& pool = pup::global_pool();
        auto file_path_sv = pool.get(file.path);

        // Skip files outside scopes (but always check Tupfiles, upstream deps,
        // and implicit dependencies like headers from .d files)
        if (!scopes.empty() && !is_tupfile(file_path_sv)
            && !pup::is_path_in_any_scope(file_path_sv, scopes)
            && !std::binary_search(upstream_files.begin(), upstream_files.end(), file_path_sv)
            && !std::binary_search(implicit_dep_files.begin(), implicit_dep_files.end(), file.path)) {
            continue;
        }

        ++metrics.files_checked;

        // File resolution:
        // All paths are now source-relative (generated files include build root, e.g., "build/program").
        auto file_path = file_path_sv;
        auto path = pup::path::is_absolute(file_path) ? file_path : pool.get(pup::path::join(source_root, file_path));
        ++metrics.stat_calls;
        auto stat_result = pup::platform::stat_file(path);

        // Overlay: parse-time inputs live under config_root but are indexed source-relative.
        if (!stat_result && !config_root.empty() && config_root != source_root
            && !pup::path::is_absolute(file_path)) {
            auto config_path = pool.get(pup::path::join(config_root, file_path));
            ++metrics.stat_calls;
            if (auto config_stat = pup::platform::stat_file(config_path)) {
                stat_result = config_stat;
                path = config_path;
            }
        }

        if (!stat_result) {
            if (verbose) {
                print("  Changed (stat failed): {}\n", file_path_sv);
            }
            ++metrics.files_changed;
            changed.push_back(pool.intern(file_path_sv));
            continue;
        }

        // Size check (fast path)
        auto current_size = stat_result->size;
        if (current_size != file.size) {
            if (verbose) {
                print("  Changed (size): {}\n", file_path_sv);
            }
            ++metrics.files_changed;
            changed.push_back(pool.intern(file_path_sv));
            continue;
        }

        // Stat cache: skip hash if size + mtime match and not racy-clean
        auto const current_mtime_ns = stat_result->mtime_ns;
        auto const cached_mtime_ns = file.mtime_ns;
        auto const is_racy_clean = save_time_ns > 0
            && cached_mtime_ns >= save_time_ns - RACY_CLEAN_THRESHOLD_NS;

        if (!no_stat_cache && cached_mtime_ns != 0 && current_mtime_ns == cached_mtime_ns && !is_racy_clean) {
            // Stat cache hit: size + mtime match, trust cached hash
            ++metrics.hashes_skipped;
            continue;
        }

        // Content hash check (authoritative)
        if (file.content_hash != pup::ZERO_HASH) {
            auto hash_result = pup::sha256_file(path);
            if (!hash_result || *hash_result != file.content_hash) {
                if (verbose) {
                    print("  Changed (hash): {}\n", file_path_sv);
                }
                ++metrics.files_changed;
                changed.push_back(pool.intern(file_path_sv));
            }
        } else {
            // ZERO_HASH indicates hash wasn't computed - treat as changed to be safe
            if (verbose) {
                print("  Changed (no hash): {}\n", file_path_sv);
            }
            ++metrics.files_changed;
            changed.push_back(pool.intern(file_path_sv));
        }
    }

    return changed;
}

/// Context for building implicit dependency entries in the index.
/// Holds mutable state shared between get_or_create_dir and create_implicit_file.
struct ImplicitDepContext {
    pup::index::Index& index;
    PathIdMap& path_to_id;
    pup::NodeId& next_id;
    pup::NodeIdPairSet& added_edges;
    std::string_view source_root;
    pup::NodeIdMap32 const& cmd_remap;
};

/// Recursively get or create directory entries in the index.
/// Returns the NodeId for the directory at dir_path.
auto get_or_create_dir(
    ImplicitDepContext& ctx,
    std::string_view dir_path
) -> pup::NodeId
{
    auto& pool = pup::global_pool();
    auto path_str = pool.get(pup::path::normalize(dir_path));

    if (path_str.empty() || path_str == ".") {
        return pup::NodeId { 0 };
    }

    if (auto it = path_id_find(ctx.path_to_id, path_str); it != nullptr) {
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
            .name = pool.intern("/"),
            .path = pool.intern("/"),
            .size = 0,
            .mtime_ns = 0,
            .content_hash = {},
        };
        ctx.index.add_file(std::move(entry));
        path_id_insert(ctx.path_to_id, pool.intern("/"), dir_id);
        return dir_id;
    }

    auto parent_path = pup::path::parent(path_str);
    auto parent_id = get_or_create_dir(ctx, parent_path);

    auto dir_id = ctx.next_id++;
    auto entry = pup::index::FileEntry {
        .id = dir_id,
        .parent_id = parent_id,
        .src_id = 0,
        .type = pup::NodeType::Directory,
        .flags = pup::NodeFlags::None,
        .name = pool.intern(pup::path::filename(path_str)),
        .path = pool.intern(path_str),
        .size = 0,
        .mtime_ns = 0,
        .content_hash = {},
    };
    ctx.index.add_file(std::move(entry));
    path_id_insert(ctx.path_to_id, pool.intern(path_str), dir_id);
    return dir_id;
}

/// Create a file entry for an implicit dependency (header file discovered by compiler).
/// Creates parent directories as needed and returns the file's NodeId.
auto create_implicit_file(
    ImplicitDepContext& ctx,
    std::string_view abs_path,
    std::string_view rel_path
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
            eprint("Warning: Failed to hash file: {}\n", pup::global_pool().get(pup::platform::to_utf8(abs_path)));
        }

        auto stat_result = pup::platform::stat_file(abs_path);
        if (stat_result) {
            file_size = stat_result->size;
            mtime_ns = stat_result->mtime_ns;
        }
    }

    auto parent_dir = pup::path::parent(rel_path);
    auto parent_id = get_or_create_dir(ctx, parent_dir);

    auto file_id = ctx.next_id++;

    auto& pool = pup::global_pool();
    auto entry = pup::index::FileEntry {
        .id = file_id,
        .parent_id = parent_id,
        .src_id = 0,
        .type = pup::NodeType::File,
        .flags = pup::NodeFlags::None,
        .name = pool.intern(pup::path::filename(rel_path)),
        .path = pool.intern(rel_path),
        .size = file_size,
        .mtime_ns = mtime_ns,
        .content_hash = content_hash,
    };
    ctx.index.add_file(std::move(entry));
    path_id_insert(ctx.path_to_id, pool.intern(rel_path), file_id);
    return file_id;
}

} // namespace

/// Serialize file and directory nodes from the build graph to the index.
/// Returns the populated index and a path-to-id mapping for later use.
auto serialize_graph_nodes(
    pup::graph::BuildGraph const& state,
    std::string_view source_root,
    std::string_view config_root,
    std::string_view output_root
) -> std::pair<pup::index::Index, PathIdMap>
{
    auto const& g = state.graph;
    auto index = pup::index::Index {};
    auto path_to_id = PathIdMap {};

    for (auto id : pup::graph::all_nodes(g)) {
        if (pup::node_id::is_command(id)) {
            continue;
        }

        auto type = pup::graph::get<pup::NodeType>(g, id);
        auto node_flags = pup::graph::get<pup::NodeFlags>(g, id);

        switch (type) {
        case pup::NodeType::File:
        case pup::NodeType::Generated: {
            auto node_path = pup::graph::get_full_path(g, id, state.path_cache);
            if (node_path.empty()) {
                // Dropping a slot would shift every later load-derived id (id == position + 1).
                index.add_file(pup::index::FileEntry {
                    .id = id,
                    .parent_id = pup::graph::get_parent_dir(g, id),
                    .src_id = 0,
                    .type = type,
                    .flags = node_flags,
                    .name = pup::graph::get<pup::graph::Name>(g, id),
                    .path = pup::StringId::Empty,
                    .size = 0,
                    .mtime_ns = 0,
                    .content_hash = {},
                });
                break;
            }

            auto fs_path = node_path;
            if (type == pup::NodeType::Generated) {
                fs_path = strip_build_root_prefix(fs_path, pup::graph::get_build_root_name(g));
            }

            auto file_path = pup::global_pool().get((type == pup::NodeType::Generated) ? pup::path::join(output_root, fs_path) : pup::path::join(source_root, node_path));

            // Overlay: parse-time inputs live under config_root but are indexed source-relative.
            if (type == pup::NodeType::File && !pup::platform::exists(file_path)
                && !config_root.empty() && config_root != source_root) {
                auto config_path = pup::global_pool().get(pup::path::join(config_root, node_path));
                if (pup::platform::exists(config_path)) {
                    file_path = config_path;
                }
            }

            auto content_hash = pup::Hash256 {};
            auto file_size = std::uint64_t { 0 };
            auto mtime_ns = std::int64_t { 0 };

            if (pup::platform::exists(file_path)) {
                auto hash_result = pup::sha256_file(file_path);
                if (hash_result) {
                    content_hash = *hash_result;
                } else {
                    eprint("Warning: Failed to hash file: {}\n", pup::global_pool().get(pup::platform::to_utf8(file_path)));
                }

                auto stat_result = pup::platform::stat_file(file_path);
                if (stat_result) {
                    file_size = stat_result->size;
                    mtime_ns = stat_result->mtime_ns;
                }
            }

            auto& pool = pup::global_pool();
            auto entry = pup::index::FileEntry {
                .id = id,
                .parent_id = pup::graph::get_parent_dir(g, id),
                .src_id = 0,
                .type = type,
                .flags = node_flags,
                .name = pup::graph::get<pup::graph::Name>(g, id),
                .path = pool.intern(node_path),
                .size = file_size,
                .mtime_ns = mtime_ns,
                .content_hash = content_hash,
            };
            index.add_file(std::move(entry));
            path_id_insert(path_to_id, pool.intern(node_path), id);
            break;
        }
        case pup::NodeType::Directory:
        case pup::NodeType::GeneratedDir: {
            auto node_path = pup::graph::get_full_path(g, id, state.path_cache);
            auto& pool = pup::global_pool();

            auto entry = pup::index::FileEntry {
                .id = id,
                .parent_id = pup::graph::get_parent_dir(g, id),
                .src_id = 0,
                .type = type,
                .flags = node_flags,
                .name = pup::graph::get<pup::graph::Name>(g, id),
                .path = pool.intern(node_path),
                .size = 0,
                .mtime_ns = 0,
                .content_hash = {},
            };
            index.add_file(std::move(entry));
            if (!node_path.empty()) {
                path_id_insert(path_to_id, pool.intern(node_path), id);
            }
            break;
        }
        case pup::NodeType::Ghost: {
            // A ghost that exists on disk but is produced by no rule is a
            // foreign input (e.g. the variant's tup.config): record its content
            // so change detection can see it.
            auto& pool = pup::global_pool();
            auto node_path = pup::graph::get_full_path(g, id, state.path_cache);

            auto path_id = pup::StringId::Empty;
            auto content_hash = pup::Hash256 {};
            auto file_size = std::uint64_t { 0 };
            auto mtime_ns = std::int64_t { 0 };

            if (!node_path.empty()) {
                auto fs_path = strip_build_root_prefix(node_path, pup::graph::get_build_root_name(g));
                auto file_path = pool.get(pup::path::join(output_root, fs_path));
                if (pup::platform::exists(file_path)) {
                    if (auto hash_result = pup::sha256_file(file_path)) {
                        content_hash = *hash_result;
                    }
                    if (auto stat_result = pup::platform::stat_file(file_path)) {
                        file_size = stat_result->size;
                        mtime_ns = stat_result->mtime_ns;
                    }
                    path_id = pool.intern(node_path);
                }
            }

            index.add_file(pup::index::FileEntry {
                .id = id,
                .parent_id = pup::graph::get_parent_dir(g, id),
                .src_id = 0,
                .type = type,
                .flags = node_flags,
                .name = pup::graph::get<pup::graph::Name>(g, id),
                .path = path_id,
                .size = file_size,
                .mtime_ns = mtime_ns,
                .content_hash = content_hash,
            });
            break;
        }
        case pup::NodeType::Variable:
        case pup::NodeType::Group:
        case pup::NodeType::Root: {
            // These node types must be in index to maintain consecutive ID sequence
            auto entry = pup::index::FileEntry {
                .id = id,
                .parent_id = pup::graph::get_parent_dir(g, id),
                .src_id = 0,
                .type = type,
                .flags = node_flags,
                .name = pup::graph::get<pup::graph::Name>(g, id),
                .path = pup::StringId::Empty,
                .size = 0,
                .content_hash = (type == pup::NodeType::Variable) ? pup::graph::get<pup::Hash256>(g, id) : pup::Hash256 {},
            };
            index.add_file(std::move(entry));
            break;
        }
        case pup::NodeType::Command:
        case pup::NodeType::Condition:
        case pup::NodeType::Phi:
            // Unreachable: all_nodes yields only file-space ids past the is_command filter.
            break;
        }
    }

    return { std::move(index), std::move(path_to_id) };
}

/// Serialize command nodes from the build graph to the index.
/// v8: Store template + operands instead of fully-expanded command.
/// Serialize commands whose guard is satisfied, assigning dense index ids.
/// Returns the graph-id -> index-position remap; guard-unsatisfied commands are
/// absent from it. A guard-unsatisfied command never ran; recording its identity
/// would mask it as known on reactivation.
auto serialize_command_nodes(
    pup::graph::BuildGraph const& state,
    pup::index::Index& index,
    PathIdMap const& path_to_id
) -> pup::NodeIdMap32
{
    auto const& g = state.graph;
    auto remap = pup::NodeIdMap32 {};
    auto next_position = std::uint32_t { 1 };
    for (auto id : pup::graph::all_nodes(g)) {
        if (!pup::node_id::is_command(id)) {
            continue;
        }
        if (!pup::graph::is_guard_satisfied(g, id)) {
            continue;
        }

        auto inputs = pup::graph::view<pup::graph::Inputs>(g, id);
        auto outputs = pup::graph::view<pup::graph::Outputs>(g, id);
        auto& pool = pup::global_pool();

        auto source_dir_sv = pool.get(pup::graph::get<pup::graph::SourceDir>(g, id));
        auto dir_id = pup::NodeId { 0 };
        if (!source_dir_sv.empty()) {
            auto it = path_id_find(path_to_id, source_dir_sv);
            if (it != nullptr) {
                dir_id = it->second;
            }
        }

        auto entry = pup::index::CommandEntry {
            .id = pup::node_id::make_command(next_position),
            .dir_id = dir_id,
            .instruction_pattern = pup::graph::get<pup::graph::InstructionPattern>(g, id),
            .display = pup::graph::get<pup::graph::Display>(g, id),
            .env = pup::StringId::Empty,
            .identity = pup::graph::compute_command_identity(g, id, state.path_cache),
            .inputs = std::move(inputs),
            .outputs = std::move(outputs),
        };
        index.add_command(std::move(entry));
        remap.set(id, next_position);
        ++next_position;
    }
    return remap;
}

/// Serialize edges from the build graph to the index, rewriting command
/// endpoints through the dense-id remap. Edges touching a command absent from
/// the remap (guard-unsatisfied) are dropped with it.
auto serialize_edges(
    pup::graph::BuildGraph const& state,
    pup::index::Index& index,
    pup::NodeIdMap32 const& cmd_remap
) -> void
{
    auto remap_endpoint = [&cmd_remap](pup::NodeId id) -> pup::NodeId {
        if (!pup::node_id::is_command(id)) {
            return id;
        }
        if (!cmd_remap.contains(id)) {
            return pup::INVALID_NODE_ID;
        }
        return pup::node_id::make_command(cmd_remap.get(id));
    };
    for (auto const& edge : state.graph.edges) {
        // Order-only edges are ephemeral — rebuilt from Tupfiles on each parse
        if (edge.type == pup::LinkType::OrderOnly) {
            continue;
        }
        auto from = remap_endpoint(edge.from);
        auto to = remap_endpoint(edge.to);
        if (from == pup::INVALID_NODE_ID || to == pup::INVALID_NODE_ID) {
            continue;
        }
        index.add_edge(pup::index::EdgeEntry {
            .from = from,
            .to = to,
            .type = edge.type,
        });
    }
}

namespace {

/// Compute the next available NodeId after all existing nodes.
auto compute_next_id(pup::graph::BuildGraph const& state) -> pup::NodeId
{
    auto max_file_id = pup::NodeId { 0 };
    for (auto id : pup::graph::all_nodes(state.graph)) {
        if (!pup::node_id::is_command(id) && id > max_file_id) {
            max_file_id = id;
        }
    }
    return pup::NodeId { max_file_id + 1 };
}

/// Process discovered implicit dependencies from compiler output.
/// Adds new file entries and implicit edges to the index.
auto process_implicit_deps(
    DiscoveredDeps const& discovered_deps,
    ImplicitDepContext& ctx
) -> void
{
    auto& pool = pup::global_pool();
    for (auto const& [graph_cmd_id, deps] : discovered_deps) {
        if (!ctx.cmd_remap.contains(graph_cmd_id)) {
            continue;
        }
        auto cmd_id = pup::node_id::make_command(ctx.cmd_remap.get(graph_cmd_id));
        for (auto dep_id_val : deps) {
            auto dep_path = pool.get(dep_id_val);
            auto abs_path = pup::path::is_absolute(dep_path) ? dep_path : pool.get(pup::path::join(ctx.source_root, dep_path));

            auto rel_path = std::string_view {};
            if (pup::is_path_under(abs_path, ctx.source_root)) {
                rel_path = pool.get(pup::path::relative(abs_path, ctx.source_root));
            } else {
                rel_path = abs_path;
            }

            auto it = path_id_find(ctx.path_to_id, rel_path);
            auto dep_id = it != nullptr
                ? it->second
                : create_implicit_file(ctx, abs_path, rel_path);

            if (ctx.added_edges.insert(dep_id, cmd_id)) {
                ctx.index.add_edge(pup::index::EdgeEntry {
                    .from = dep_id,
                    .to = cmd_id,
                    .type = pup::LinkType::Implicit,
                });
            }
        }
    }
}

/// Preserve implicit edges from the old index for commands that weren't rebuilt.
auto preserve_old_implicit_edges(
    pup::index::Index const& old_index,
    DiscoveredDeps const& discovered_deps,
    ImplicitDepContext& ctx
) -> void
{
    auto& pool = pup::global_pool();
    auto commands_with_new_deps = pup::NodeIdMap32 {};
    for (auto const& [graph_cmd_id, _] : discovered_deps) {
        if (ctx.cmd_remap.contains(graph_cmd_id)) {
            commands_with_new_deps.set(pup::node_id::make_command(ctx.cmd_remap.get(graph_cmd_id)), 1);
        }
    }

    // Map a command's structural identity -> its id in the new index. Command ids are
    // positional and shift across builds (e.g. when an earlier-created command is
    // removed), so the old edge's `to` id cannot be trusted to mean the same command.
    // Identity is stable, so we re-resolve each carried edge's command through it.
    auto hash_less = [](pup::Hash256 const& a, pup::Hash256 const& b) {
        return std::memcmp(a.data(), b.data(), a.size()) < 0;
    };
    auto identity_to_new_id = pup::Vec<std::pair<pup::Hash256, pup::NodeId>> {};
    identity_to_new_id.reserve(ctx.index.commands().size());
    for (auto const& cmd : ctx.index.commands()) {
        identity_to_new_id.emplace_back(cmd.identity, cmd.id);
    }
    std::sort(identity_to_new_id.begin(), identity_to_new_id.end(), [&](auto const& a, auto const& b) { return hash_less(a.first, b.first); });

    for (auto const& edge : old_index.edges()) {
        if (edge.type != pup::LinkType::Implicit) {
            continue;
        }

        // Re-resolve the old command to its identity-matched counterpart in the new
        // index. If it's gone or its definition changed (no identity match), drop the
        // edge: the command either no longer exists or rebuilt and rediscovered its deps.
        auto const* old_cmd = old_index.find_command_by_id(edge.to);
        if (!old_cmd) {
            continue;
        }
        auto match = std::lower_bound(identity_to_new_id.begin(), identity_to_new_id.end(), old_cmd->identity, [&](auto const& p, pup::Hash256 const& key) { return hash_less(p.first, key); });
        if (match == identity_to_new_id.end() || match->first != old_cmd->identity) {
            continue;
        }
        auto new_to_id = match->second;

        if (commands_with_new_deps.contains(new_to_id)) {
            continue;
        }

        auto const* old_file = old_index.find_file_by_id(edge.from);
        if (!old_file) {
            continue;
        }

        auto old_file_path = pup::global_pool().get(old_file->path);
        auto new_file_it = path_id_find(ctx.path_to_id, old_file_path);
        auto abs_path = pup::path::is_absolute(old_file_path) ? old_file_path : pool.get(pup::path::join(ctx.source_root, old_file_path));
        auto new_from_id = new_file_it != nullptr
            ? new_file_it->second
            : create_implicit_file(ctx, abs_path, old_file_path);

        if (ctx.added_edges.insert(new_from_id, new_to_id)) {
            ctx.index.add_edge(pup::index::EdgeEntry {
                .from = new_from_id,
                .to = new_to_id,
                .type = pup::LinkType::Implicit,
            });
        }
    }
}

auto hash_less(pup::Hash256 const& a, pup::Hash256 const& b) -> bool
{
    return std::memcmp(a.data(), b.data(), a.size()) < 0;
}

/// Sorted (identity → graph NodeId) map: the cross-build join key for commands.
using IdentityMap = Vec<std::pair<pup::Hash256, pup::NodeId>>;

auto build_identity_map(pup::graph::BuildGraph const& state) -> IdentityMap
{
    auto map = IdentityMap {};
    for (auto id : pup::graph::all_nodes(state.graph)) {
        if (pup::node_id::is_command(id)) {
            map.emplace_back(
                pup::graph::compute_command_identity(state.graph, id, state.path_cache), id
            );
        }
    }
    std::sort(map.begin(), map.end(), [](auto const& a, auto const& b) { return hash_less(a.first, b.first); });
    return map;
}

auto find_by_identity(IdentityMap const& map, pup::Hash256 const& identity) -> std::optional<pup::NodeId>
{
    auto const* it = std::lower_bound(
        map.begin(), map.end(), identity, [](auto const& p, auto const& k) { return hash_less(p.first, k); }
    );
    if (it == map.end() || std::memcmp(it->first.data(), identity.data(), identity.size()) != 0) {
        return std::nullopt;
    }
    return it->second;
}

auto expand_implicit_deps(
    pup::Vec<StringId> const& changed,
    pup::index::Index const& index,
    pup::graph::BuildGraph const& state,
    IdentityMap const& identity_map
) -> pup::Vec<StringId>
{
    auto result = pup::Vec<StringId> { changed };
    auto added = Vec<StringId> {};
    added.reserve(changed.size());
    for (auto const& s : changed) {
        added.push_back(s);
    }
    std::sort(added.begin(), added.end());

    // Build sorted path -> file pointer map
    auto path_to_file = Vec<std::pair<StringId, pup::index::FileEntry const*>> {};
    path_to_file.reserve(index.files().size());
    for (auto const& file : index.files()) {
        if (!pup::is_empty(file.path)) {
            path_to_file.emplace_back(file.path, &file);
        }
    }
    std::sort(path_to_file.begin(), path_to_file.end());

    // Build edge index using NodeIdArenaIndex pattern (from_id -> edge pointers)
    // Use a sorted vector of (from_id, edge*) for grouped lookup
    auto edges_by_from = Vec<std::pair<pup::NodeId, pup::index::EdgeEntry const*>> {};

    for (auto const& edge : index.edges()) {
        if (edge.type == pup::LinkType::Implicit || edge.type == pup::LinkType::Sticky) {
            edges_by_from.emplace_back(edge.from, &edge);
        }
    }
    std::sort(edges_by_from.begin(), edges_by_from.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

    for (auto const& path : changed) {
        auto it = std::lower_bound(path_to_file.begin(), path_to_file.end(), path, [](auto const& p, auto const& k) { return p.first < k; });
        if (it == path_to_file.end() || it->first != path) {
            continue;
        }

        auto file_id = pup::NodeId { it->second->id };
        auto edge_lo = std::lower_bound(edges_by_from.begin(), edges_by_from.end(), file_id, [](auto const& p, auto const& k) { return p.first < k; });

        for (auto edge_it = edge_lo; edge_it != edges_by_from.end() && edge_it->first == file_id; ++edge_it) {
            auto const* edge = edge_it->second;
            auto cmd_id = pup::NodeId { edge->to };
            auto const* cmd = index.find_command_by_id(cmd_id);
            if (!cmd) {
                continue;
            }

            auto cmd_node_id = find_by_identity(identity_map, cmd->identity);
            if (!cmd_node_id) {
                continue;
            }

            for (auto output_id : pup::graph::get_outputs(state.graph, *cmd_node_id)) {
                auto output_path_sv = pup::graph::get_full_path(state.graph, output_id, state.path_cache);
                if (!output_path_sv.empty()) {
                    auto output_path_id = pup::global_pool().intern(output_path_sv);
                    if (!std::binary_search(added.begin(), added.end(), output_path_id)) {
                        auto pos = std::lower_bound(added.begin(), added.end(), output_path_id);
                        added.insert(pos, output_path_id);
                        result.push_back(output_path_id);
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
    pup::graph::BuildGraph const& state,
    DiscoveredDeps const& discovered_deps,
    std::string_view source_root,
    std::string_view config_root,
    std::string_view output_root,
    pup::index::Index const* old_index = nullptr
) -> pup::index::Index
{
    // Serialize file/directory nodes from the build graph
    auto [index, path_to_id] = serialize_graph_nodes(state, source_root, config_root, output_root);

    auto cmd_remap = serialize_command_nodes(state, index, path_to_id);

    serialize_edges(state, index, cmd_remap);

    auto next_id = compute_next_id(state);
    auto added_edges = pup::NodeIdPairSet {};
    auto ctx = ImplicitDepContext {
        .index = index,
        .path_to_id = path_to_id,
        .next_id = next_id,
        .added_edges = added_edges,
        .source_root = source_root,
        .cmd_remap = cmd_remap,
    };

    // Process discovered implicit dependencies from compiler output
    process_implicit_deps(discovered_deps, ctx);

    // Preserve implicit edges from the old index for commands that weren't rebuilt
    if (old_index) {
        preserve_old_implicit_edges(*old_index, discovered_deps, ctx);
    }

    return std::move(index);
}

/// Validate output targets exist in the build graph.
/// Returns node IDs on success, or empty optional with error printed on failure.
auto validate_output_targets(
    pup::Vec<pup::StringId> const& targets,
    pup::graph::BuildGraph const& state,
    std::string_view variant_name,
    bool verbose
) -> std::optional<pup::Vec<pup::NodeId>>
{
    auto& pool = pup::global_pool();
    auto node_ids = pup::Vec<pup::NodeId> {};
    for (auto target_id : targets) {
        auto target_sv = pool.get(target_id);
        auto node_id = std::optional<pup::NodeId> {};
        if (auto pid = state.graph.paths.find_path(target_sv, pool, pup::PathId::BuildRoot)) {
            if (auto const* hit = state.graph.path_to_node.find(pup::to_underlying(*pid))) {
                node_id = pup::NodeId { *hit };
            }
        }
        if (!node_id) {
            veprint(variant_name, "Error: {} is not in build graph\n", target_sv);
            return std::nullopt;
        }
        if (pup::graph::get<pup::NodeType>(state.graph, *node_id) != pup::NodeType::Generated) {
            veprint(variant_name, "Error: {} is not a build output\n", target_sv);
            return std::nullopt;
        }
        node_ids.push_back(*node_id);
        if (verbose) {
            vprint(variant_name, "Output target: {}\n", target_sv);
        }
    }
    return node_ids;
}

struct NewCommands {
    pup::Vec<StringId> changed_outputs;
    pup::Vec<pup::NodeId> forced_cmds;
};

/// Detect new commands (in graph but not index). Their outputs join the changed-file
/// set; a command that contributes no output paths cannot be reached through that
/// currency, so it is returned for direct scheduling instead.
auto detect_new_commands(
    pup::graph::BuildGraph const& state,
    pup::index::Index const& idx,
    std::string_view variant_name,
    bool verbose
) -> NewCommands
{
    auto const& g = state.graph;
    auto result = NewCommands {};

    // Identity-keyed membership: a command must (re)build when its structural identity
    // is absent from the previous index. Identity folds command text together with the
    // values of the vars it depends on, so a change invisible to the rendered string
    // (e.g. an exported env var the subprocess reads via $VAR) still flips the identity.
    auto old_identities = pup::Vec<pup::Hash256> {};
    old_identities.reserve(idx.commands().size());
    for (auto const& cmd : idx.commands()) {
        old_identities.push_back(cmd.identity);
    }
    std::sort(old_identities.begin(), old_identities.end(), hash_less);

    for (auto id : pup::graph::all_nodes(g)) {
        if (!pup::node_id::is_command(id)) {
            continue;
        }
        if (!pup::graph::is_guard_satisfied(g, id)) {
            continue;
        }
        auto identity = pup::graph::compute_command_identity(g, id, state.path_cache);
        if (!std::binary_search(old_identities.begin(), old_identities.end(), identity, hash_less)) {
            auto pushed_outputs = false;
            for (auto output_id : pup::graph::get_outputs(g, id)) {
                auto output_path_sv = pup::graph::get_full_path(g, output_id, state.path_cache);
                if (!output_path_sv.empty()) {
                    result.changed_outputs.push_back(pup::global_pool().intern(output_path_sv));
                    pushed_outputs = true;
                }
            }
            if (!pushed_outputs) {
                result.forced_cmds.push_back(id);
            }
            if (verbose) {
                auto display_sv = pup::global_pool().get(pup::graph::get<pup::graph::Display>(g, id));
                vprint(variant_name, "  New command: {}\n", display_sv);
            }
        }
    }
    return result;
}

/// Remove stale outputs from removed commands and report them.
auto contains_id(pup::Vec<pup::StringId> const& v, pup::StringId id) -> bool
{
    return !pup::is_empty(id) && std::find(v.begin(), v.end(), id) != v.end();
}

auto remove_stale_outputs(
    pup::index::Index const& idx,
    IdentityMap const& identity_map,
    pup::Vec<pup::StringId> const& parse_scopes,
    pup::Vec<pup::StringId> const& parsed_dirs,
    pup::Vec<pup::StringId> const& available_dirs,
    std::string_view source_root,
    std::string_view variant_name,
    bool dry_run,
    bool verbose
) -> void
{
    auto& pool = pup::global_pool();
    for (auto const& cmd : idx.commands()) {
        // Only delete outputs of a command whose directory we have authoritative
        // knowledge of this run. A dir is authoritative if we successfully parsed
        // its Tupfile (so the graph is the source of truth for it), or if it has
        // no Tupfile at all (deleted) and lies within the build scope (so its rules
        // are genuinely gone). A dir we merely discovered but did not parse — out
        // of scope, or a parse failure under --keep-going — tells us nothing about
        // whether its commands were removed, so we preserve. The root's dir_id is 0
        // (empty path); it keys as "." to match how parsing records it.
        auto const* dir_file = idx.find_file_by_id(cmd.dir_id);
        auto dir_path = (dir_file && !pup::is_empty(dir_file->path)) ? pool.get(dir_file->path) : std::string_view { "." };
        auto dir_id = (dir_file && !pup::is_empty(dir_file->path)) ? dir_file->path : pool.find(".");
        auto const in_scope = parse_scopes.empty() || pup::is_path_in_any_scope(dir_path, parse_scopes);
        auto const authoritative = contains_id(parsed_dirs, dir_id)
            || (!contains_id(available_dirs, dir_id) && in_scope);
        if (!authoritative) {
            continue;
        }

        if (find_by_identity(identity_map, cmd.identity)) {
            continue;
        }

        for (auto const* edge : idx.edges_from(cmd.id)) {
            auto const* file = idx.find_file_by_id(edge->to);
            if (!file || file->type != pup::NodeType::Generated) {
                continue;
            }

            // Paths now include build root (e.g., "build/program")
            auto file_path_sv = pup::global_pool().get(file->path);
            auto abs_path = pup::global_pool().get(pup::path::join(source_root, file_path_sv));
            if (pup::platform::exists(abs_path)) {
                if (dry_run) {
                    vprint(variant_name, "Would remove stale: {}\n", file_path_sv);
                } else {
                    if (pup::platform::remove_file(abs_path)) {
                        if (verbose) {
                            vprint(variant_name, "  Removed stale: {}\n", file_path_sv);
                        }
                    }
                }
            }
        }

        if (verbose) {
            vprint(variant_name, "  Removed command: {}\n", pup::global_pool().get(cmd.display));
        }
    }
}

auto intersect_filters(
    pup::NodeIdMap32 const& a,
    pup::NodeIdMap32 const& b
) -> pup::NodeIdMap32
{
    auto result = pup::NodeIdMap32 {};
    auto const& smaller = (a.size() <= b.size()) ? a : b;
    auto const& larger = (a.size() <= b.size()) ? b : a;

    struct Ctx {
        pup::NodeIdMap32 const* other;
        pup::NodeIdMap32* result;
    };
    auto ctx = Ctx { &larger, &result };

    smaller.for_each_id(
        [](pup::NodeId id, void* c) {
            auto* x = static_cast<Ctx*>(c);
            if (x->other->contains(id)) {
                x->result->set(id, 1);
            }
        },
        &ctx
    );

    return result;
}

struct BuildFilter {
    pup::NodeIdMap32 set;
    bool active = false;

    auto intersect_with(pup::NodeIdMap32 next) -> void
    {
        if (active) {
            set = intersect_filters(set, next);
        } else {
            set = std::move(next);
            active = true;
        }
    }

    auto ptr() const -> pup::NodeIdMap32 const*
    {
        return active ? &set : nullptr;
    }
};

/// Build a single variant with the given options.
/// Expects opts.build_dirs to contain at most one element.
auto build_single_variant(
    Options const& opts,
    std::string_view variant_name
) -> int
{
    auto variant_start = pup::SteadyClock::now();
    auto scanner_registry = make_scanner_registry();
    auto* const scanner_ptr = scanner_registry ? &*scanner_registry : nullptr;
    if (scanner_ptr && opts.verbose) {
        vprint(variant_name, "Implicit dependency tracking enabled\n");
    }

    auto layout = discover_layout(make_layout_options(opts));
    if (!layout) {
        veprint(variant_name, "Error: {}\n", layout.error().msg());
        return EXIT_FAILURE;
    }
    auto scopes = compute_build_scopes(opts, *layout);

    // Only scope parsing when explicit targets are given.
    // CWD-derived scoping should still parse all Tupfiles so that
    // out-of-scope Tupfile changes are detected for incremental builds.
    // When -a is set, always parse all Tupfiles so that cross-directory
    // producers are discovered and ghost nodes get resolved.
    auto parse_scopes = (opts.targets.empty() || opts.include_all_deps)
        ? pup::Vec<pup::StringId> {}
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

    auto result = build_context(opts, ctx_opts);
    // build_context loads the old index as part of its work; that span is timed
    // separately, so discount it here to keep the phases disjoint.
    pup::thread_metrics().parse_time = std::chrono::duration_cast<std::chrono::microseconds>(pup::SteadyClock::now() - variant_start)
        - pup::thread_metrics().index_load_time;
    if (!result) {
        veprint(variant_name, "Error: {}\n", result.error().msg());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;
    auto& bs = ctx.graph();
    auto num_commands = std::size_t { pup::graph::nodes_of_type(bs.graph, pup::NodeType::Command).size() };

    if (num_commands == 0) {
        vprint(variant_name, "Nothing to do.\n");
        return EXIT_SUCCESS;
    }

    auto target_ids_result = validate_output_targets(
        opts.output_targets,
        bs,
        variant_name,
        opts.verbose
    );
    if (!target_ids_result) {
        return EXIT_FAILURE;
    }
    auto target_node_ids = std::move(*target_ids_result);

    auto& pool = pup::global_pool();
    auto source_root_str = pool.get(ctx.layout().source_root);
    auto config_root_str = pool.get(ctx.layout().config_root);

    auto index_path = pup::global_pool().get(ctx.layout().index_path());
    auto const* old_idx_ptr = ctx.old_index();
    auto use_incremental = false;
    auto changed_files = pup::Vec<StringId> {};
    auto forced_cmds = pup::Vec<pup::NodeId> {};

    if (old_idx_ptr) {
        auto const& idx = *old_idx_ptr;

        // Build the identity → NodeId map: the cross-build join key for commands.
        // Must happen after parsing (operands set) but before incremental logic.
        auto cmd_index_start = pup::SteadyClock::now();
        auto const identity_map = build_identity_map(bs);
        auto cmd_index_elapsed = pup::SteadyClock::now() - cmd_index_start;
        pup::thread_metrics().command_index_time = std::chrono::duration_cast<std::chrono::microseconds>(cmd_index_elapsed);

        auto upstream_files = Vec<std::string_view> {};
        if (opts.include_all_deps && !scopes.empty()) {
            upstream_files = pup::graph::collect_upstream_files(bs, scopes);
        }

        // Always include implicit deps (headers from .d files) for in-scope
        // commands, even if the headers live outside the scoped directories.
        auto implicit_dep_files = Vec<StringId> {};
        if (!scopes.empty()) {
            implicit_dep_files = collect_implicit_dep_files(idx, scopes);
        }
        if (opts.verbose) {
            if (scopes.empty()) {
                vprint(variant_name, "Full project build\n");
            } else {
                vprint(variant_name, "Scoped build:");
                for (auto scope_id : scopes) {
                    auto scope_sv = pup::global_pool().get(scope_id);
                    print(" {}", scope_sv);
                }
                if (opts.include_all_deps) {
                    print(" (+{} upstream deps)", upstream_files.size());
                }
                if (!implicit_dep_files.empty()) {
                    print(" (+{} implicit deps)", implicit_dep_files.size());
                }
                print("\n");
            }
        }

        auto change_detect_start = pup::SteadyClock::now();
        changed_files = find_changed_files_with_implicit(
            source_root_str,
            config_root_str,
            idx,
            scopes,
            upstream_files,
            implicit_dep_files,
            collect_inactive_output_paths(bs),
            opts.verbose,
            opts.no_stat_cache
        );
        auto change_detect_elapsed = pup::SteadyClock::now() - change_detect_start;
        pup::thread_metrics().change_detection_time = std::chrono::duration_cast<std::chrono::microseconds>(change_detect_elapsed);

        auto implicit_deps_start = pup::SteadyClock::now();
        changed_files = expand_implicit_deps(changed_files, idx, bs, identity_map);
        auto implicit_deps_elapsed = pup::SteadyClock::now() - implicit_deps_start;
        pup::thread_metrics().implicit_deps_time = std::chrono::duration_cast<std::chrono::microseconds>(implicit_deps_elapsed);

        auto new_cmds_start = pup::SteadyClock::now();
        auto new_cmds = detect_new_commands(bs, idx, variant_name, opts.verbose);
        auto new_cmds_elapsed = pup::SteadyClock::now() - new_cmds_start;
        pup::thread_metrics().new_commands_time = std::chrono::duration_cast<std::chrono::microseconds>(new_cmds_elapsed);
        for (auto& f : new_cmds.changed_outputs) {
            changed_files.push_back(std::move(f));
        }
        forced_cmds = std::move(new_cmds.forced_cmds);

        auto stale_start = pup::SteadyClock::now();
        remove_stale_outputs(
            idx,
            identity_map,
            parse_scopes,
            ctx.parsed_dirs(),
            ctx.available_dirs(),
            source_root_str,
            variant_name,
            opts.dry_run,
            opts.verbose
        );
        auto stale_elapsed = pup::SteadyClock::now() - stale_start;
        pup::thread_metrics().stale_outputs_time = std::chrono::duration_cast<std::chrono::microseconds>(stale_elapsed);

        if (changed_files.empty() && forced_cmds.empty()) {
            vprint(variant_name, "Nothing to do (up to date).\n");
            if (opts.stat) {
                print_stats(idx, num_commands, 0, variant_start);
            }
            return EXIT_SUCCESS;
        }

        use_incremental = true;
        if (opts.verbose) {
            vprint(variant_name, "Incremental build: {} changed files\n", changed_files.size());
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

    auto scheduler = pup::exec::Scheduler { std::move(sched_opts) };
    auto discovered_deps = DiscoveredDeps {};

    auto use_tty_progress = pup::stdout_is_tty() && !opts.verbose && !opts.dry_run;
    auto progress = pup::exec::ProgressState { .total = num_commands };
    auto prev_lines = std::size_t { 0 };

    scheduler.on_job_start([&](pup::exec::BuildJob const& job) {
        auto& pool = pup::global_pool();
        if (opts.verbose || opts.dry_run) {
            auto display_sv = pool.get(job.display);
            vprint(variant_name, "{}\n", display_sv);
        } else if (use_tty_progress) {
            auto target = job.outputs.empty() ? job.display : job.outputs.front();
            progress = pup::exec::job_started(std::move(progress), job.id, target);
            auto output = pup::exec::render_tty(progress, variant_name);
            pup::exec::display_progress(output, prev_lines);
        }
    });

    scheduler.on_job_complete([&](pup::exec::BuildJob const& job, pup::exec::JobResult const& job_result) {
        auto& pool = pup::global_pool();
        if (!job_result.success) {
            if (use_tty_progress) {
                pup::exec::finalize_progress(prev_lines);
            }
            auto display_sv = pool.get(job.display);
            veprint(variant_name, "FAILED: {}\n", display_sv);
            if (!pup::is_empty(job_result.output)) {
                auto output_sv = pool.get(job_result.output);
                eprint("{}\n", output_sv);
            }
        }

        if (!job_result.discovered_deps.empty()) {
            auto target_id = job_result.deps_for_command != pup::INVALID_NODE_ID
                ? job_result.deps_for_command
                : job.id;
            auto& deps = discovered_deps_get(discovered_deps, target_id);

            auto source_root_sv = pool.get(ctx.layout().source_root);
            for (auto dep_id : job_result.discovered_deps) {
                auto dep_sv = pool.get(dep_id);
                auto working_dir_sv = pool.get(job.working_dir);
                auto to_resolve = pup::path::is_absolute(dep_sv)
                    ? dep_sv
                    : pool.get(pup::path::join(working_dir_sv, dep_sv));
                auto resolved_result = pup::platform::canonical(to_resolve);
                if (!resolved_result) {
                    if (opts.verbose) {
                        eprint("Warning: Skipping dependency '{}': {}\n", dep_sv, resolved_result.error().msg());
                    }
                    continue;
                }
                auto resolved_sv = pool.get(*resolved_result);

                if (pup::is_path_under(resolved_sv, source_root_sv)) {
                    auto rel_sv = pool.get(pup::path::relative(resolved_sv, source_root_sv));
                    if (rel_sv.starts_with("..")) {
                        if (opts.verbose) {
                            eprint("Warning: Cannot relativize '{}'\n", resolved_sv);
                        }
                        continue;
                    }
                    deps.push_back(pool.intern(rel_sv));
                } else {
                    deps.push_back(*resolved_result);
                }
            }
        }

        if (use_tty_progress) {
            progress = pup::exec::job_completed(std::move(progress), job.id, job_result.success);
            auto output = pup::exec::render_tty(progress, variant_name);
            pup::exec::display_progress(output, prev_lines);
        } else if (!opts.verbose && !opts.dry_run) {
            progress = pup::exec::job_completed(std::move(progress), job.id, job_result.success);
            print("\r{} ", pup::global_pool().get(pup::exec::render_simple(progress, variant_name)));
            flush(Stream::Out);
        }
    });

    scheduler.on_progress([&](std::size_t /* done */, std::size_t total) {
        progress.total = total;
    });

    // Identify config-generating commands to exclude from regular build
    // (config rules should only run during 'pup configure')
    auto config_cmds = find_config_commands(bs, pup::global_pool().get(ctx.layout().source_root));
    auto config_cmd_ids = NodeIdMap32 {};
    for (auto const& cfg : config_cmds) {
        config_cmd_ids.set(cfg.cmd_id, 1);
    }

    auto start = pup::SteadyClock::time_point { pup::SteadyClock::now() };

    // Composable filter: layer independent concerns, intersect when combined
    auto filter = BuildFilter {};

    if (use_incremental) {
        auto affected = pup::graph::collect_affected_commands(bs.graph, changed_files);
        for (auto id : forced_cmds) {
            affected.set(id, 1);
        }
        filter.intersect_with(std::move(affected));
    }

    if (!target_node_ids.empty()) {
        filter.intersect_with(pup::graph::collect_required_commands(bs.graph, target_node_ids));
    }

    if (opts.include_all_deps && !scopes.empty() && !use_incremental) {
        auto scope_cmds = pup::graph::collect_scope_with_upstream_commands(bs.graph, scopes);
        for (auto const& cfg : config_cmds) {
            scope_cmds.remove(cfg.cmd_id);
        }
        filter.intersect_with(std::move(scope_cmds));
    }

    // Exclude config-generating commands (they run during configure, not build)
    if (!config_cmds.empty()) {
        auto non_config = pup::NodeIdMap32 {};
        for (auto id : pup::graph::all_nodes(bs.graph)) {
            if (node_id::is_command(id) && !config_cmd_ids.contains(id)) {
                non_config.set(id, 1);
            }
        }
        filter.intersect_with(std::move(non_config));
    }

    auto build_result = scheduler.build(bs, filter.ptr());
    auto end = pup::SteadyClock::time_point { pup::SteadyClock::now() };
    auto duration = std::chrono::milliseconds { std::chrono::duration_cast<std::chrono::milliseconds>(end - start) };
    // The scheduler times its own job-list construction; the rest of the span is execution.
    pup::thread_metrics().exec_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start)
        - pup::thread_metrics().job_list_time;

    if (use_tty_progress) {
        pup::exec::finalize_progress(prev_lines);
    } else if (!opts.verbose && !opts.dry_run) {
        print("\n");
    }

    if (!build_result) {
        veprint(variant_name, "Build failed: {}\n", build_result.error().msg());
        return EXIT_FAILURE;
    }

    auto const& stats = *build_result;
    if (stats.completed_jobs == 0 && stats.failed_jobs == 0) {
        vprint(variant_name, "Nothing to do.\n");
    } else if (stats.failed_jobs > 0) {
        vprint(variant_name, "Build completed: {} commands ({} failed) in {}ms\n", stats.completed_jobs, stats.failed_jobs, duration.count());
    } else {
        vprint(variant_name, "Build completed: {} commands in {}ms\n", stats.completed_jobs, duration.count());
    }

    auto final_index = std::optional<pup::index::Index> {};
    if (!opts.dry_run) {
        // Save index even after partial failures - successful outputs are recorded
        // so they won't be rebuilt. Failed outputs don't exist, so stat will fail
        // and they'll be detected as changed on next build.
        auto output_root_str = pup::global_pool().get(ctx.layout().output_root);
        auto index_rebuild_start = pup::SteadyClock::time_point { pup::SteadyClock::now() };
        auto index = pup::index::Index { build_index(
            bs,
            discovered_deps,
            source_root_str,
            config_root_str,
            output_root_str,
            old_idx_ptr
        ) };

        auto index_save_start = pup::SteadyClock::time_point { pup::SteadyClock::now() };
        pup::thread_metrics().index_rebuild_time = std::chrono::duration_cast<std::chrono::microseconds>(index_save_start - index_rebuild_start);
        auto write_result = pup::Result<void> { pup::index::write_index(index_path, index) };
        auto index_save_end = pup::SteadyClock::time_point { pup::SteadyClock::now() };
        pup::thread_metrics().index_save_time = std::chrono::duration_cast<std::chrono::microseconds>(index_save_end - index_save_start);

        if (!write_result) {
            veprint(variant_name, "Warning: Failed to save index: {}\n", write_result.error().msg());
        } else if (opts.verbose) {
            vprint(variant_name, "Saved index: {} files, {} commands, {} edges\n", index.file_count(), index.command_count(), index.edge_count());
        }
        final_index = std::move(index);
    }

    if (opts.stat) {
        if (final_index) {
            print_stats(*final_index, num_commands, stats.completed_jobs, variant_start);
        } else if (old_idx_ptr) {
            print_stats(*old_idx_ptr, num_commands, stats.completed_jobs, variant_start);
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
