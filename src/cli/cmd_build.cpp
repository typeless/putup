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
#include "pup/parser/ignore.hpp"
#include "pup/platform/file_io.hpp"

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
    std::string_view index_path,
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
    if (auto index_stat = pup::platform::stat_file(index_path)) {
        print("  Index size:         {} bytes\n", Pad { index_stat->size, 6 });
    }
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

/// Collect the content-carrying inputs (`edge_mask::inputs`) of in-scope commands: the files
/// the scope filter must not skip even though they live outside the scoped directories.
auto collect_scope_crossing_inputs(
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

    // Derived from link_role rather than enumerated here, so a new link type joins this
    // bypass by being classified once instead of by every walk remembering it — the omission
    // that made a declared source input invisible to a scoped build (#200, #189). Ordering
    // is excluded by the role, not by hand: order-only means existence, not content.
    for (auto const& edge : index.edges()) {
        if (!pup::graph::in_mask(edge.type, pup::graph::edge_mask::inputs)) {
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

    std::sort(result.begin(), result.end(), pup::handle_less);
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

    std::sort(result.begin(), result.end(), pup::handle_less);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

auto find_changed_files_with_implicit(
    std::string_view source_root,
    std::string_view config_root,
    std::string_view build_root,
    pup::index::Index const& old_index,
    pup::Vec<pup::StringId> const& scopes,
    pup::parser::IgnoreList const& excludes,
    Vec<std::string_view> const& upstream_files,
    Vec<StringId> const& scope_crossing_inputs,
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
        if (std::binary_search(inactive_outputs.begin(), inactive_outputs.end(), file.path, pup::handle_less)) {
            continue;
        }

        auto& pool = pup::global_pool();
        auto file_path_sv = pool.get(file.path);

        // Generated paths carry the build-root prefix; scopes are source-relative
        auto scope_path_sv = file.type == pup::NodeType::Generated
            ? pool.get(pup::strip_path_prefix(file_path_sv, build_root))
            : file_path_sv;

        // Skip files outside scopes (but always check Tupfiles, upstream deps,
        // and implicit dependencies like headers from .d files)
        if (!scopes.empty() && !is_tupfile(file_path_sv)
            && !pup::is_path_in_any_scope(scope_path_sv, scopes)
            && !std::binary_search(upstream_files.begin(), upstream_files.end(), file_path_sv)
            && !std::binary_search(scope_crossing_inputs.begin(), scope_crossing_inputs.end(), file.path, pup::handle_less)) {
            continue;
        }

        // Excluded dirs are not parsed this run, so nothing can consume their
        // changes; observing them would record state we have no authority over.
        if (excludes.is_ignored_dir(pup::path::parent(file_path_sv))) {
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
        if (!stat_result && file.type == pup::NodeType::File
            && !config_root.empty() && config_root != source_root
            && !pup::path::is_absolute(file_path)) {
            auto config_path = pool.get(pup::path::join(config_root, file_path));
            ++metrics.stat_calls;
            if (auto config_stat = pup::platform::stat_file(config_path)) {
                stat_result = config_stat;
                path = config_path;
            }
        }

        if (!stat_result) {
            // Still gone, and the build that deleted it already routed that to its consumers;
            // reporting it again is what ran them a second time (#213). Only absence is
            // discharged — if it comes back, the checks below see it like any other change.
            if (pup::has_flag(file.flags, pup::NodeFlags::Deleted)) {
                continue;
            }
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
    /// Files written by commands that ran in this build, by index id. A consumer that reads one
    /// of these with nothing ordering it after the producer may have read it before it existed,
    /// and the dep is then recorded from a post-run stat — as already satisfied (#274).
    pup::NodeIdMap32 const& produced_this_build;
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
            eprint("Warning: Failed to hash file: {}\n", abs_path);
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
    std::string_view output_root,
    pup::Vec<pup::StringId> const& deleted_stale
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
            if (type == pup::NodeType::File && !config_root.empty()
                && config_root != source_root && !pup::platform::exists(file_path)) {
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
                    eprint("Warning: Failed to hash file: {}\n", file_path);
                }

                auto stat_result = pup::platform::stat_file(file_path);
                if (stat_result) {
                    file_size = stat_result->size;
                    mtime_ns = stat_result->mtime_ns;
                }
            }

            auto& pool = pup::global_pool();
            // Records that this build already routed the file's absence, so the next one does
            // not read the same stat failure as news. The slot has to stay: id == position + 1.
            auto entry_flags = std::binary_search(deleted_stale.begin(), deleted_stale.end(), pool.intern(node_path), pup::handle_less)
                ? node_flags | pup::NodeFlags::Deleted
                : node_flags;
            auto entry = pup::index::FileEntry {
                .id = id,
                .parent_id = pup::graph::get_parent_dir(g, id),
                .src_id = 0,
                .type = type,
                .flags = entry_flags,
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
    PathIdMap const& path_to_id,
    pup::NodeIdMap32 const& must_rerun_cmds
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
            .key = pup::graph::compute_command_key(g, id, state.path_cache),
            .signature = pup::graph::compute_command_signature(g, id, state.path_cache),
            .must_rerun = must_rerun_cmds.contains(id),
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

/// Whether anything in the recorded edges puts `cmd` after `dep`: walk forward from `dep` over
/// every edge kind the scheduler orders by. A command reached this way cannot have run before
/// `dep` existed, so it did not race it (#274). Reaching `cmd` from its own output — a depfile
/// naming its target — counts, which is what keeps a malformed .d from marking forever.
auto orders_after(pup::index::Index const& index, pup::NodeId dep, pup::NodeId cmd) -> bool
{
    auto seen = pup::NodeIdMap32 {};
    auto queue = pup::Vec<pup::NodeId> {};
    // From the producer, not the file: a generator's other output is what orders the consumer,
    // and that path leaves the file backwards before going forward.
    for (auto const* edge : index.edges_to(dep)) {
        if (pup::node_id::is_command(edge->from) && !seen.contains(edge->from)) {
            seen.set(edge->from, 1);
            queue.push_back(edge->from);
        }
    }
    if (!seen.contains(dep)) {
        seen.set(dep, 1);
        queue.push_back(dep);
    }

    while (!queue.empty()) {
        auto const id = queue.back();
        queue.pop_back();
        if (id == cmd) {
            return true;
        }
        for (auto const* edge : index.edges_from(id)) {
            if (!seen.contains(edge->to)) {
                seen.set(edge->to, 1);
                queue.push_back(edge->to);
            }
        }
    }
    return false;
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

            // Only when nothing orders this command after the file. Ordering is transitive —
            // a codegen emitting one declared and one discovered output orders its consumer
            // through the declared one — so this asks reachability, not adjacency: enumerating
            // path shapes is how the two previous attempts at this taxed correct builds.
            if (ctx.produced_this_build.contains(dep_id) && !orders_after(ctx.index, dep_id, cmd_id)) {
                for (auto& entry : ctx.index.commands()) {
                    if (entry.id == cmd_id) {
                        entry.must_rerun = true;
                        break;
                    }
                }
            }

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

/// How a set of commands is addressed for joining, in one place so that every join --
/// graph to index, index to graph, index to index -- agrees on what "the same command"
/// means. A command that produces files is addressed by the files: output ownership is
/// unique among guard-satisfied commands, enforced where the output edge is created, so a
/// produced path names its producer and survives any edit to the recipe. A command that
/// produces nothing has no such name and is addressed by its textual key instead.
///
/// The two are exclusive. A command that produced files is never looked up by key, or the
/// two directions of a join would disagree about whether two commands correspond.
struct CommandLookup {
    Vec<std::pair<StringId, pup::NodeId>> by_output; ///< produced path -> producer
    Vec<std::pair<pup::Hash256, pup::NodeId>> by_key;
};

/// What one command answers to. Empty outputs is what selects the textual key.
struct CommandAddress {
    Vec<StringId> outputs;
    pup::Hash256 key = {};
};

auto sort_lookup(CommandLookup& lookup) -> void
{
    std::sort(lookup.by_output.begin(), lookup.by_output.end(), [](auto const& a, auto const& b) { return pup::handle_less(a.first, b.first); });
    std::sort(lookup.by_key.begin(), lookup.by_key.end(), [](auto const& a, auto const& b) { return pup::hash_less(a.first, b.first); });
}

auto graph_command_address(pup::graph::BuildGraph const& state, pup::NodeId id) -> CommandAddress
{
    auto address = CommandAddress {};
    for (auto output_id : pup::graph::get_outputs(state.graph, id)) {
        auto path_sv = pup::graph::get_full_path(state.graph, output_id, state.path_cache);
        if (!path_sv.empty()) {
            address.outputs.push_back(pup::global_pool().intern(path_sv));
        }
    }
    if (address.outputs.empty()) {
        address.key = pup::graph::compute_command_key(state.graph, id, state.path_cache);
    }
    return address;
}

auto index_command_address(pup::index::Index const& idx, pup::index::CommandEntry const& cmd) -> CommandAddress
{
    auto address = CommandAddress { .outputs = {}, .key = cmd.key };
    for (auto const* edge : idx.edges_from(cmd.id)) {
        auto const* file = idx.find_file_by_id(edge->to);
        if (file && file->type == pup::NodeType::Generated && !pup::is_empty(file->path)) {
            address.outputs.push_back(file->path);
        }
    }
    return address;
}

/// Guard-satisfied only, matching the set the index records.
auto graph_command_lookup(pup::graph::BuildGraph const& state) -> CommandLookup
{
    auto lookup = CommandLookup {};
    for (auto id : pup::graph::all_nodes(state.graph)) {
        if (!pup::node_id::is_command(id) || !pup::graph::is_guard_satisfied(state.graph, id)) {
            continue;
        }
        auto address = graph_command_address(state, id);
        for (auto path : address.outputs) {
            lookup.by_output.emplace_back(path, id);
        }
        if (address.outputs.empty()) {
            lookup.by_key.emplace_back(address.key, id);
        }
    }
    sort_lookup(lookup);
    return lookup;
}

auto index_command_lookup(pup::index::Index const& idx) -> CommandLookup
{
    auto lookup = CommandLookup {};
    for (auto const& cmd : idx.commands()) {
        auto address = index_command_address(idx, cmd);
        for (auto path : address.outputs) {
            lookup.by_output.emplace_back(path, cmd.id);
        }
        if (address.outputs.empty()) {
            lookup.by_key.emplace_back(address.key, cmd.id);
        }
    }
    sort_lookup(lookup);
    return lookup;
}

auto find_joined(CommandLookup const& lookup, CommandAddress const& address) -> std::optional<pup::NodeId>
{
    for (auto path : address.outputs) {
        auto const* it = std::lower_bound(
            lookup.by_output.begin(), lookup.by_output.end(), path, [](auto const& p, StringId k) { return pup::handle_less(p.first, k); }
        );
        if (it != lookup.by_output.end() && it->first == path) {
            return it->second;
        }
    }
    if (!address.outputs.empty()) {
        return std::nullopt;
    }

    auto const* it = std::lower_bound(
        lookup.by_key.begin(), lookup.by_key.end(), address.key, [](auto const& p, auto const& k) { return pup::hash_less(p.first, k); }
    );
    if (it != lookup.by_key.end() && pup::hash_equal(it->first, address.key)) {
        return it->second;
    }
    return std::nullopt;
}

/// Whether any command in the lookup still produces this path.
auto has_live_producer(CommandLookup const& lookup, StringId path) -> bool
{
    auto const* it = std::lower_bound(
        lookup.by_output.begin(), lookup.by_output.end(), path, [](auto const& p, StringId k) { return pup::handle_less(p.first, k); }
    );
    return it != lookup.by_output.end() && it->first == path;
}

auto find_joined_command(
    CommandLookup const& lookup,
    pup::index::Index const& idx,
    pup::index::CommandEntry const& cmd
) -> std::optional<pup::NodeId>
{
    return find_joined(lookup, index_command_address(idx, cmd));
}

/// Preserve implicit edges from the old index for commands that weren't rebuilt.
auto preserve_old_implicit_edges(
    pup::index::Index const& old_index,
    pup::Vec<pup::NodeId> const& executed_cmds,
    ImplicitDepContext& ctx
) -> void
{
    auto& pool = pup::global_pool();
    auto commands_that_ran = pup::NodeIdMap32 {};
    for (auto graph_cmd_id : executed_cmds) {
        if (ctx.cmd_remap.contains(graph_cmd_id)) {
            commands_that_ran.set(pup::node_id::make_command(ctx.cmd_remap.get(graph_cmd_id)), 1);
        }
    }

    // Command ids are positional and shift across builds (e.g. when an earlier-created
    // command is removed), so the old edge's `to` id cannot be trusted to mean the same
    // command; re-resolve each carried edge's command through the same join every other
    // consumer uses.
    auto const new_lookup = index_command_lookup(ctx.index);

    for (auto const& edge : old_index.edges()) {
        if (edge.type != pup::LinkType::Implicit) {
            continue;
        }

        // If the command is gone, drop the edge. If it survived and ran, the branch below
        // drops it too: whatever that run reported is now the whole truth, empty included.
        auto const* old_cmd = old_index.find_command_by_id(edge.to);
        if (!old_cmd) {
            continue;
        }
        auto joined = find_joined(new_lookup, index_command_address(old_index, *old_cmd));
        if (!joined) {
            continue;
        }
        auto new_to_id = *joined;

        if (commands_that_ran.contains(new_to_id)) {
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

auto contains_id(pup::Vec<pup::StringId> const& v, pup::StringId id) -> bool
{
    return !pup::is_empty(id) && std::find(v.begin(), v.end(), id) != v.end();
}

/// Whether this run has authoritative knowledge of a directory's rules: its
/// Tupfile was successfully parsed (the graph is the source of truth for it),
/// or it has no Tupfile at all (deleted) and lies within the build scope (so
/// its rules are genuinely gone). A dir merely discovered but not parsed — out
/// of scope, or a parse failure under --keep-going — tells us nothing about
/// whether its commands were removed. The root's dir_id is 0 (empty path); it
/// keys as "." to match how parsing records it.
auto is_dir_authoritative(
    pup::index::Index const& idx,
    pup::NodeId dir_id,
    pup::Vec<pup::StringId> const& parse_scopes,
    pup::parser::IgnoreList const& excludes,
    pup::Vec<pup::StringId> const& parsed_dirs,
    pup::Vec<pup::StringId> const& available_dirs,
    pup::Vec<pup::StringId> const& pruned_dirs
) -> bool
{
    auto& pool = pup::global_pool();
    auto const* dir_file = idx.find_file_by_id(dir_id);
    auto dir_path = (dir_file && !pup::is_empty(dir_file->path)) ? pool.get(dir_file->path) : std::string_view { "." };
    auto dir_str_id = (dir_file && !pup::is_empty(dir_file->path)) ? dir_file->path : pool.find(".");
    if (contains_id(parsed_dirs, dir_str_id)) {
        return true;
    }
    // A dir under a pruned nested-project root has a Tupfile this run never
    // saw; its absence from available_dirs does not mean it was deleted.
    if (pup::is_path_in_any_scope(dir_path, pruned_dirs)) {
        return false;
    }
    auto const in_scope = (parse_scopes.empty() || pup::is_path_in_any_scope(dir_path, parse_scopes))
        && !excludes.is_ignored_dir(dir_path);
    return !contains_id(available_dirs, dir_str_id) && in_scope;
}

/// Carry forward old-index commands from directories this run has no
/// authoritative knowledge of, so a scoped build's saved index still describes
/// the whole project (issue #125). Copies each command with its operand files
/// (stat data preserved) and every edge except Implicit — OrderOnly among them,
/// which carries removal routing for order-only inputs — remapping ids to the new
/// index's dense sequences; implicit edges are re-attached afterwards by
/// preserve_old_implicit_edges through the same join.
auto merge_out_of_scope_commands(
    pup::index::Index const& old_index,
    pup::Vec<pup::StringId> const& parse_scopes,
    pup::parser::IgnoreList const& excludes,
    pup::Vec<pup::StringId> const& parsed_dirs,
    pup::Vec<pup::StringId> const& available_dirs,
    pup::Vec<pup::StringId> const& pruned_dirs,
    ImplicitDepContext& ctx
) -> void
{
    auto& pool = pup::global_pool();

    auto new_lookup = index_command_lookup(ctx.index);

    // serialize_graph_nodes registers File/Generated/Directory paths but not
    // Ghosts; without this the merge would duplicate a ghost's entry by path.
    auto ghost_paths = PathIdMap {};
    for (auto const& file : ctx.index.files()) {
        if (!pup::is_empty(file.path) && path_id_find(ctx.path_to_id, pool.get(file.path)) == nullptr) {
            path_id_insert(ghost_paths, file.path, file.id);
        }
    }

    auto find_new_id_by_path = [&](std::string_view path_sv) -> pup::NodeId {
        if (auto const* it = path_id_find(ctx.path_to_id, path_sv); it != nullptr) {
            return it->second;
        }
        if (auto const* it = path_id_find(ghost_paths, path_sv); it != nullptr) {
            return it->second;
        }
        return pup::INVALID_NODE_ID;
    };

    auto old_to_new_file = pup::NodeIdMap32 {};
    auto resolve_file = [&](pup::NodeId old_id) -> pup::NodeId {
        if (old_to_new_file.contains(old_id)) {
            return pup::NodeId { old_to_new_file.get(old_id) };
        }
        auto const* old_file = old_index.find_file_by_id(old_id);
        if (!old_file || pup::is_empty(old_file->path)) {
            return pup::INVALID_NODE_ID;
        }
        auto path_sv = pool.get(old_file->path);
        auto new_id = pup::NodeId {};
        if (auto existing = find_new_id_by_path(path_sv); existing != pup::INVALID_NODE_ID) {
            new_id = existing;
        } else {
            auto parent_id = get_or_create_dir(ctx, pup::path::parent(path_sv));
            new_id = ctx.next_id++;
            ctx.index.add_file(pup::index::FileEntry {
                .id = new_id,
                .parent_id = parent_id,
                .src_id = 0,
                .type = old_file->type,
                .flags = old_file->flags,
                .name = old_file->name,
                .path = old_file->path,
                .size = old_file->size,
                .mtime_ns = old_file->mtime_ns,
                .content_hash = old_file->content_hash,
            });
            path_id_insert(ctx.path_to_id, old_file->path, new_id);
        }
        old_to_new_file.set(old_id, new_id);
        return new_id;
    };

    // Content that moved, not content that vanished (#247): the record stops claiming currency, not ownership.
    auto dep_state_changed = [&](pup::NodeId old_id) -> bool {
        auto const* old_file = old_index.find_file_by_id(old_id);
        if (!old_file || pup::is_empty(old_file->path)) {
            return false;
        }
        auto new_id = find_new_id_by_path(pool.get(old_file->path));
        if (new_id == pup::INVALID_NODE_ID) {
            return false;
        }
        auto const* new_file = ctx.index.find_file_by_id(new_id);
        return new_file && new_file->content_hash != old_file->content_hash;
    };
    auto any_dep_changed = [&](pup::index::CommandEntry const& cmd) -> bool {
        for (auto id : cmd.inputs) {
            if (dep_state_changed(id)) {
                return true;
            }
        }
        for (auto id : cmd.outputs) {
            if (dep_state_changed(id)) {
                return true;
            }
        }
        for (auto const* edge : old_index.edges_to(cmd.id)) {
            if (dep_state_changed(edge->from)) {
                return true;
            }
        }
        return false;
    };

    auto old_to_new_cmd = pup::NodeIdMap32 {};
    for (auto const& cmd : old_index.commands()) {
        if (is_dir_authoritative(old_index, cmd.dir_id, parse_scopes, excludes, parsed_dirs, available_dirs, pruned_dirs)) {
            continue;
        }
        if (find_joined(new_lookup, index_command_address(old_index, cmd))) {
            continue;
        }
        // Marked, not dropped: dropping retracts which outputs this command owns along with the
        // claim that they are current, and only the second is in doubt (#241).
        auto must_rerun = cmd.must_rerun || any_dep_changed(cmd);

        // Omitted, not dropped (#243): an unresolvable operand is one this record cannot
        // describe, but the outputs that did resolve are still owned by nothing else.
        auto unresolved = false;
        auto new_dir_id = pup::NodeId { 0 };
        if (cmd.dir_id != pup::NodeId { 0 }) {
            new_dir_id = resolve_file(cmd.dir_id);
            if (new_dir_id == pup::INVALID_NODE_ID) {
                new_dir_id = pup::NodeId { 0 };
                unresolved = true;
            }
        }

        auto resolve_operands = [&](pup::Vec<pup::NodeId> const& old_ids, pup::Vec<pup::NodeId>& out) -> void {
            for (auto old_id : old_ids) {
                auto id = resolve_file(old_id);
                if (id == pup::INVALID_NODE_ID) {
                    unresolved = true;
                    continue;
                }
                out.push_back(id);
            }
        };
        auto new_inputs = pup::Vec<pup::NodeId> {};
        auto new_outputs = pup::Vec<pup::NodeId> {};
        resolve_operands(cmd.inputs, new_inputs);
        resolve_operands(cmd.outputs, new_outputs);
        // An operand it could not carry means the record no longer describes what ran, so it
        // keeps its outputs but stops claiming they are current.
        must_rerun = must_rerun || unresolved;

        auto new_cmd_id = pup::node_id::make_command(static_cast<std::uint32_t>(ctx.index.commands().size()) + 1);
        ctx.index.add_command(pup::index::CommandEntry {
            .id = new_cmd_id,
            .dir_id = new_dir_id,
            .instruction_pattern = cmd.instruction_pattern,
            .display = cmd.display,
            .env = cmd.env,
            .key = cmd.key,
            .signature = cmd.signature,
            .must_rerun = must_rerun,
            .inputs = std::move(new_inputs),
            .outputs = std::move(new_outputs),
        });
        old_to_new_cmd.set(cmd.id, new_cmd_id);
    }

    for (auto const& edge : old_index.edges()) {
        // Implicit edges are carried by preserve_old_implicit_edges instead.
        if (edge.type == pup::LinkType::Implicit) {
            continue;
        }
        auto from_is_merged = pup::node_id::is_command(edge.from) && old_to_new_cmd.contains(edge.from);
        auto to_is_merged = pup::node_id::is_command(edge.to) && old_to_new_cmd.contains(edge.to);
        if (!from_is_merged && !to_is_merged) {
            continue;
        }
        auto remap_endpoint = [&](pup::NodeId id) -> pup::NodeId {
            if (pup::node_id::is_command(id)) {
                return old_to_new_cmd.contains(id) ? pup::NodeId { old_to_new_cmd.get(id) } : pup::INVALID_NODE_ID;
            }
            return resolve_file(id);
        };
        auto from = remap_endpoint(edge.from);
        auto to = remap_endpoint(edge.to);
        if (from == pup::INVALID_NODE_ID || to == pup::INVALID_NODE_ID) {
            continue;
        }
        if (ctx.added_edges.insert(from, to)) {
            ctx.index.add_edge(pup::index::EdgeEntry {
                .from = from,
                .to = to,
                .type = edge.type,
            });
        }
    }
}

/// Walk index edges from a file to the commands that consume it, reporting each one's joined
/// graph id. Crossing non-command nodes is what makes a group hop need no special case, and
/// stopping at commands is what keeps a command's own output edges out of the walk (#169).
template<typename Fn>
auto for_each_consuming_command(
    pup::index::Index const& idx,
    CommandLookup const& join,
    pup::NodeId start,
    pup::graph::LinkTypeMask mask,
    Fn&& emit
) -> void
{
    auto frontier = pup::Vec<pup::NodeId> { start };
    auto crossed = pup::NodeIdMap32 {};
    while (!frontier.empty()) {
        auto from = frontier.back();
        frontier.pop_back();
        for (auto const* edge : idx.edges_from(from)) {
            if (!pup::graph::in_mask(edge->type, mask)) {
                continue;
            }
            auto to = pup::NodeId { edge->to };
            if (auto const* cmd = idx.find_command_by_id(to)) {
                if (auto cmd_node_id = find_joined_command(join, idx, *cmd)) {
                    emit(*cmd_node_id);
                }
                continue;
            }
            if (crossed.contains(to)) {
                continue;
            }
            crossed.set(to, 1);
            frontier.push_back(to);
        }
    }
}

/// What routing a change produces: paths to treat as changed, and the commands that have no
/// output path to carry one. Shared by every router, so none can report only the first half.
struct RoutingDelta {
    pup::Vec<StringId> changed_paths;
    pup::Vec<pup::NodeId> forced_cmds;
};

/// Outputs, not the command itself, are the currency dependents propagate through; a command
/// declaring none has no path to carry, so it is forced directly.
template<typename PushPath>
auto propagate_command_effect(
    pup::graph::BuildGraph const& state,
    pup::NodeId cmd_id,
    PushPath push_path,
    pup::Vec<pup::NodeId>& forced_cmds
) -> void
{
    auto has_output_path = false;
    for (auto output_id : pup::graph::get_outputs(state.graph, cmd_id)) {
        auto output_path_sv = pup::graph::get_full_path(state.graph, output_id, state.path_cache);
        if (output_path_sv.empty()) {
            continue;
        }
        // Set before push_path decides: a caller that dedups an already-changed path has still
        // found an output, and forcing the command as well would be wrong.
        has_output_path = true;
        push_path(pup::global_pool().intern(output_path_sv));
    }
    if (!has_output_path) {
        forced_cmds.push_back(cmd_id);
    }
}

auto expand_implicit_deps(
    pup::Vec<StringId> const& changed,
    pup::index::Index const& index,
    pup::graph::BuildGraph const& state,
    CommandLookup const& join
) -> RoutingDelta
{
    auto result = RoutingDelta { .changed_paths = pup::Vec<StringId> { changed }, .forced_cmds = {} };
    auto added = Vec<StringId> {};
    added.reserve(changed.size());
    for (auto const& s : changed) {
        added.push_back(s);
    }
    std::sort(added.begin(), added.end(), pup::handle_less);

    // Build sorted path -> file pointer map
    auto path_to_file = Vec<std::pair<StringId, pup::index::FileEntry const*>> {};
    path_to_file.reserve(index.files().size());
    for (auto const& file : index.files()) {
        if (!pup::is_empty(file.path)) {
            path_to_file.emplace_back(file.path, &file);
        }
    }
    std::sort(path_to_file.begin(), path_to_file.end(), [](auto const& a, auto const& b) { return pup::handle_less(a.first, b.first); });

    for (auto const& path : changed) {
        auto it = std::lower_bound(path_to_file.begin(), path_to_file.end(), path, [](auto const& p, auto const& k) { return pup::handle_less(p.first, k); });
        if (it == path_to_file.end() || it->first != path) {
            continue;
        }

        for_each_consuming_command(
            index,
            join,
            pup::NodeId { it->second->id },
            pup::graph::edge_mask::discovered_consumers,
            [&](pup::NodeId cmd_node_id) {
                propagate_command_effect(
                    state, cmd_node_id, [&](StringId output_path_id) {
                        auto pos = std::lower_bound(added.begin(), added.end(), output_path_id, pup::handle_less);
                        if (pos != added.end() && *pos == output_path_id) {
                            return;
                        }
                        added.insert(pos, output_path_id);
                        result.changed_paths.push_back(output_path_id);
                    },
                    result.forced_cmds
                );
            }
        );
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
    pup::index::Index const* old_index = nullptr,
    pup::Vec<pup::StringId> const& parse_scopes = {},
    pup::parser::IgnoreList const& excludes = {},
    pup::Vec<pup::StringId> const& parsed_dirs = {},
    pup::Vec<pup::StringId> const& available_dirs = {},
    pup::Vec<pup::StringId> const& pruned_dirs = {},
    pup::NodeIdMap32 const& must_rerun_cmds = {},
    pup::Vec<pup::NodeId> const& executed_cmds = {},
    pup::Vec<pup::StringId> const& deleted_stale = {}
) -> pup::index::Index
{
    // Serialize file/directory nodes from the build graph
    auto [index, path_to_id] = serialize_graph_nodes(state, source_root, config_root, output_root, deleted_stale);

    auto cmd_remap = serialize_command_nodes(state, index, path_to_id, must_rerun_cmds);

    serialize_edges(state, index, cmd_remap);

    auto next_id = compute_next_id(state);
    auto added_edges = pup::NodeIdPairSet {};

    auto produced_this_build = pup::NodeIdMap32 {};
    for (auto graph_cmd_id : executed_cmds) {
        if (!cmd_remap.contains(graph_cmd_id)) {
            continue;
        }
        auto const cmd_id = pup::node_id::make_command(cmd_remap.get(graph_cmd_id));
        for (auto const* edge : index.edges_from(cmd_id)) {
            if (!pup::node_id::is_command(edge->to)) {
                produced_this_build.set(edge->to, 1);
            }
        }
    }

    auto ctx = ImplicitDepContext {
        .index = index,
        .path_to_id = path_to_id,
        .next_id = next_id,
        .added_edges = added_edges,
        .source_root = source_root,
        .cmd_remap = cmd_remap,
        .produced_this_build = produced_this_build,
    };

    // Process discovered implicit dependencies from compiler output
    process_implicit_deps(discovered_deps, ctx);

    // After the discovered deps, so a merge-created copy of an old entry cannot shadow the
    // fresh one this build just stat'd — a carried NodeFlags::Deleted would then discharge a
    // later real deletion as already routed, and the consumer would never run (#237). Still
    // before preserve_old_implicit_edges, which re-attaches carried edges by identity and so
    // must see the merged records; that, not the ordering against the deps, is the constraint.
    if (old_index) {
        merge_out_of_scope_commands(*old_index, parse_scopes, excludes, parsed_dirs, available_dirs, pruned_dirs, ctx);
    }

    // Preserve implicit edges from the old index for commands that weren't rebuilt
    if (old_index) {
        preserve_old_implicit_edges(*old_index, executed_cmds, ctx);
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
    pup::Vec<pup::NodeId> must_rerun; ///< Recorded as needing to run; still do unless they succeed now
};

/// A rule may not produce a file that already exists as a source. Upstream tup rejects it
/// ("Attempting to insert '<f>' as a generated node when it already exists as a different
/// type"); putup used to let the generated node win, and in-tree that overwrote the tracked
/// file on disk. The previous index is what tells a source file apart from our own output
/// sitting in the source tree after an in-tree build -- exactly the distinction tup's node
/// types carry.
auto reject_shadowed_sources(
    pup::graph::BuildGraph const& state,
    pup::index::Index const* old_index,
    std::string_view source_root,
    std::string_view config_root
) -> pup::Result<void>
{
    auto const& g = state.graph;
    auto& pool = pup::global_pool();
    auto build_root_name = pup::graph::get_build_root_name(g);

    // Only in-tree builds can destroy anything: out-of-tree, %o resolves under the build
    // root, so a rule whose output path collides with a committed file writes beside it
    // rather than over it. The collision is still confusing there -- the source becomes
    // unreadable through that path -- but it is not data loss, and rejecting on it fails
    // builds that cannot hurt anyone (a second build dir sees the first one's artifacts).
    if (!build_root_name.empty()) {
        return {};
    }

    // Two questions putup can answer for certain. With no previous index nothing we
    // produced can be on disk yet, so anything sitting at an output's path is a source.
    // With one, a path it recorded as a source File is a source whatever is on disk now.
    // Everything else -- notably a generated file the previous index does not mention,
    // which is what a scoped build leaves behind for out-of-scope outputs -- is not
    // decidable from here and is left alone.
    auto recorded_sources = pup::Vec<StringId> {};
    if (old_index != nullptr) {
        for (auto const& file : old_index->files()) {
            if (file.type == pup::NodeType::File && !pup::is_empty(file.path)) {
                recorded_sources.push_back(file.path);
            }
        }
        std::sort(recorded_sources.begin(), recorded_sources.end(), pup::handle_less);
    }

    // Guard-satisfied producers only, like every other command walk here: an inactive
    // conditional branch declares outputs it will never write, and rejecting on those
    // fails projects that build fine.
    for (auto cmd_id : pup::graph::all_nodes(g)) {
        if (!pup::node_id::is_command(cmd_id) || !pup::graph::is_guard_satisfied(g, cmd_id)) {
            continue;
        }
        for (auto id : pup::graph::get_outputs(g, cmd_id)) {
            auto full_path_sv = pup::graph::get_full_path(g, id, state.path_cache);
            if (full_path_sv.empty()) {
                continue;
            }
            auto rel_sv = pool.get(pup::strip_path_prefix(full_path_sv, build_root_name));
            // The two-stage configure design has a rule produce the tup.config that the same
            // build then reads as configuration. Match the whole basename: a suffix test also
            // exempts anything merely ending in those characters, e.g. mytup.config.
            if (rel_sv == "tup.config" || rel_sv.ends_with("/tup.config")) {
                continue;
            }

            // On disk in either input tree. Without this the printed remedy -- delete the file
            // and try again -- would not clear the error, because the index still records it.
            auto abs_sv = pool.get(pup::path::join(source_root, rel_sv));
            auto on_disk = pup::platform::exists(abs_sv);
            if (!on_disk && !config_root.empty() && config_root != source_root) {
                on_disk = pup::platform::exists(pool.get(pup::path::join(config_root, rel_sv)));
            }
            if (!on_disk) {
                continue;
            }

            auto rel_id = pool.intern(rel_sv);
            if (old_index != nullptr
                && !std::binary_search(recorded_sources.begin(), recorded_sources.end(), rel_id, pup::handle_less)) {
                continue;
            }

            auto err = pup::Buf {};
            err.fmt(
                "Attempting to create '{}' as a generated file when it already exists as a source "
                "file. You can do one of two things to fix this:\n"
                "  1) If this file is really supposed to be created from the command, delete the "
                "file from the filesystem and try again.\n"
                "  2) Change your rule in the Tupfile so you aren't trying to overwrite the file.",
                rel_sv
            );
            return pup::make_error<void>(pup::ErrorCode::DuplicateNode, err.view());
        }
    }
    return {};
}

/// A ghost is a path no rule produces. Nothing consuming it is harmless, and one backed by a
/// file on disk is a foreign input like tup.config; the rest are inputs the project declares
/// and cannot supply. Runs before the up-to-date short-circuit, so an ill-formed project is
/// rejected on every build rather than only on one that has work to do.
auto reject_unresolved_ghosts(
    pup::graph::BuildGraph const& state,
    std::string_view output_root,
    bool all_deps_would_help
) -> pup::Result<void>
{
    auto const& g = state.graph;
    auto& pool = pup::global_pool();
    auto build_root_name = pup::graph::get_build_root_name(g);

    for (auto id : pup::graph::nodes_of_type(g, pup::NodeType::Ghost)) {
        auto consumed = false;
        pup::graph::edges_for_each(
            g, id, pup::graph::EdgeDirection::Forward, pup::graph::edge_mask::consumers, [&consumed](pup::NodeId) { consumed = true; }
        );
        if (!consumed) {
            continue;
        }

        auto path_sv = pup::graph::get_full_path(g, id, state.path_cache);
        auto lookup_path = path_sv;
        auto build_prefix = pup::Buf {};
        build_prefix += build_root_name;
        build_prefix += '/';
        if (!build_root_name.empty() && path_sv.starts_with(build_prefix.view())) {
            lookup_path = path_sv.substr(build_prefix.size());
        }
        if (pup::platform::exists(pool.get(pup::path::join(output_root, lookup_path)))) {
            continue;
        }

        auto err = pup::Buf {};
        err.fmt("Missing input file (unresolved ghost): {}\n", path_sv);
        // -a only pulls in rules this build left out of scope; when nothing was left out,
        // offering it sends the user round the same failure (#222).
        err.append(all_deps_would_help ? "  Hint: try building with -a to include upstream dependencies" : "  Hint: no rule in this build produces it — the rule that did may have been removed");
        return pup::make_error<void>(pup::ErrorCode::ParseError, err.view());
    }
    return {};
}

/// Commands that must run because of what they are rather than because a file changed:
/// no counterpart in the previous build, or a counterpart whose signature differs. Their
/// outputs join the changed-file set; a command that contributes no output paths cannot
/// be reached through that currency, so it is returned for direct scheduling instead.
///
/// The join runs graph -> index, the mirror of find_joined_command: a produced path names
/// its producer, and output-less commands fall back to the textual key.
auto detect_new_commands(
    pup::graph::BuildGraph const& state,
    pup::index::Index const& idx,
    std::string_view variant_name,
    bool verbose
) -> NewCommands
{
    auto const& g = state.graph;
    auto result = NewCommands {};

    auto old_by_output = pup::Vec<std::pair<StringId, pup::index::CommandEntry const*>> {};
    auto old_by_key = pup::Vec<std::pair<pup::Hash256, pup::index::CommandEntry const*>> {};
    for (auto const& cmd : idx.commands()) {
        auto produced_any = false;
        for (auto const* edge : idx.edges_from(cmd.id)) {
            auto const* file = idx.find_file_by_id(edge->to);
            if (file && file->type == pup::NodeType::Generated && !pup::is_empty(file->path)) {
                old_by_output.emplace_back(file->path, &cmd);
                produced_any = true;
            }
        }
        if (!produced_any) {
            old_by_key.emplace_back(cmd.key, &cmd);
        }
    }
    std::sort(old_by_output.begin(), old_by_output.end(), [](auto const& a, auto const& b) { return pup::handle_less(a.first, b.first); });
    std::sort(old_by_key.begin(), old_by_key.end(), [](auto const& a, auto const& b) { return pup::hash_less(a.first, b.first); });

    for (auto id : pup::graph::all_nodes(g)) {
        if (!pup::node_id::is_command(id)) {
            continue;
        }
        if (!pup::graph::is_guard_satisfied(g, id)) {
            continue;
        }

        auto output_paths = pup::Vec<StringId> {};
        for (auto output_id : pup::graph::get_outputs(g, id)) {
            auto output_path_sv = pup::graph::get_full_path(g, output_id, state.path_cache);
            if (!output_path_sv.empty()) {
                output_paths.push_back(pup::global_pool().intern(output_path_sv));
            }
        }

        pup::index::CommandEntry const* previous = nullptr;
        for (auto path_id : output_paths) {
            auto const* it = std::lower_bound(
                old_by_output.begin(), old_by_output.end(), path_id, [](auto const& p, StringId k) { return pup::handle_less(p.first, k); }
            );
            if (it != old_by_output.end() && it->first == path_id) {
                previous = it->second;
                break;
            }
        }
        if (!previous && output_paths.empty()) {
            auto key = pup::graph::compute_command_key(g, id, state.path_cache);
            auto const* it = std::lower_bound(
                old_by_key.begin(), old_by_key.end(), key, [](auto const& p, auto const& k) { return pup::hash_less(p.first, k); }
            );
            if (it != old_by_key.end() && pup::hash_equal(it->first, key)) {
                previous = it->second;
            }
        }

        // A record that is not evidence outranks the signature: the command must run again even
        // when nothing about it changed, and its outputs must be treated as changed so consumers
        // of whatever it half-wrote, or never wrote, are rescheduled too.
        auto signature = pup::graph::compute_command_signature(g, id, state.path_cache);
        auto must_rerun = previous != nullptr && previous->must_rerun;
        if (must_rerun) {
            result.must_rerun.push_back(id);
        }
        if (previous && !must_rerun && pup::hash_equal(previous->signature, signature)) {
            continue;
        }

        for (auto path_id : output_paths) {
            result.changed_outputs.push_back(path_id);
        }
        if (output_paths.empty()) {
            result.forced_cmds.push_back(id);
        }
        if (verbose) {
            auto display_sv = pup::global_pool().get(pup::graph::command_label(g, id, state.path_cache));
            auto reason = must_rerun ? "Last run not verified" : (previous ? "Changed command" : "New command");
            vprint(variant_name, "  {}: {}\n", reason, display_sv);
        }
    }
    return result;
}

/// Reconcile the previous input set against the current one. Change detection
/// walks the index while routing walks the graph, so a file in only one of them
/// falls through both: an added file has no index entry to compare against, and a
/// removed one has no graph edge back to its consumer. Inputs the rendered text
/// names (`%f`) escape via command identity; order-only and `%f`-less ones do not.
auto reconcile_input_set(
    pup::graph::BuildGraph const& state,
    pup::index::Index const& idx,
    pup::Vec<StringId> const& changed,
    CommandLookup const& join,
    std::string_view variant_name,
    bool verbose
) -> RoutingDelta
{
    auto const& g = state.graph;
    auto result = RoutingDelta {};

    // Every typed path, so "left the graph" below means gone, not merely not-a-source:
    // a generated file the user deleted is still a graph node and must not read as removed.
    auto graph_paths = pup::Vec<StringId> {};
    auto new_sources = pup::Vec<StringId> {};
    for (auto id : pup::graph::all_nodes(g)) {
        if (pup::node_id::is_command(id)) {
            continue;
        }
        auto path_sv = pup::graph::get_full_path(g, id, state.path_cache);
        if (path_sv.empty()) {
            continue;
        }
        auto path_id = pup::global_pool().intern(path_sv);
        graph_paths.push_back(path_id);
        if (pup::graph::get<pup::NodeType>(g, id) == pup::NodeType::File) {
            new_sources.push_back(path_id);
        }
    }
    std::sort(graph_paths.begin(), graph_paths.end(), pup::handle_less);

    auto index_files = pup::Vec<std::pair<StringId, pup::NodeId>> {};
    index_files.reserve(idx.files().size());
    for (auto const& file : idx.files()) {
        if (!pup::is_empty(file.path)) {
            index_files.emplace_back(file.path, file.id);
        }
    }
    std::sort(index_files.begin(), index_files.end(), [](auto const& a, auto const& b) { return pup::handle_less(a.first, b.first); });

    auto find_indexed = [&](StringId path_id) -> pup::index::FileEntry const* {
        auto it = std::lower_bound(
            index_files.begin(), index_files.end(), path_id, [](auto const& entry, StringId key) { return pup::handle_less(entry.first, key); }
        );
        return (it != index_files.end() && it->first == path_id) ? idx.find_file_by_id(it->second) : nullptr;
    };

    for (auto path_id : new_sources) {
        if (find_indexed(path_id)) {
            continue;
        }
        result.changed_paths.push_back(path_id);
        if (verbose) {
            vprint(variant_name, "  New file: {}\n", pup::global_pool().get(path_id));
        }
    }

    auto orphaned = pup::Vec<pup::NodeId> {};
    for (auto path_id : changed) {
        if (std::binary_search(graph_paths.begin(), graph_paths.end(), path_id, pup::handle_less)) {
            continue;
        }
        auto const* file = find_indexed(path_id);
        if (!file) {
            continue;
        }
        for_each_consuming_command(
            idx, join, pup::NodeId { file->id }, pup::graph::edge_mask::parsed_consumers, [&](pup::NodeId cmd_node_id) {
                orphaned.push_back(cmd_node_id);
                if (verbose) {
                    vprint(
                        variant_name, "  Removed input: {} ({})\n", pup::global_pool().get(path_id), pup::global_pool().get(pup::graph::command_label(g, cmd_node_id, state.path_cache))
                    );
                }
            }
        );
    }
    // Explicit comparator: libc++ extern-templates std::__sort for unsigned int*,
    // so the default form links against a libc++ we deliberately do not have.
    std::sort(orphaned.begin(), orphaned.end(), [](pup::NodeId a, pup::NodeId b) { return a < b; });
    orphaned.erase(std::unique(orphaned.begin(), orphaned.end()), orphaned.end());

    for (auto cmd_id : orphaned) {
        propagate_command_effect(
            state, cmd_id, [&](StringId path) { result.changed_paths.push_back(path); }, result.forced_cmds
        );
    }
    return result;
}

/// What a pass of stale-output cleanup found: the outputs it deleted, and whether any recorded
/// command joined no graph command. The second is what makes writing the index worthwhile on a
/// build with nothing to run -- retiring the record is the only way it stops being reported.
struct StaleCleanup {
    pup::Vec<pup::StringId> deleted;
    bool retired_commands = false;
};

/// Remove stale outputs from removed commands and report them.
///
/// Fails on the first output it cannot remove: the record naming that output is retired by not
/// being written, so a build that gave up on the file and carried on would leave nothing that
/// knows the file should not be there (#246).
auto remove_stale_outputs(
    pup::index::Index const& idx,
    CommandLookup const& join,
    pup::Vec<pup::StringId> const& parse_scopes,
    pup::parser::IgnoreList const& excludes,
    pup::Vec<pup::StringId> const& parsed_dirs,
    pup::Vec<pup::StringId> const& available_dirs,
    pup::Vec<pup::StringId> const& pruned_dirs,
    std::string_view source_root,
    std::string_view variant_name,
    bool dry_run,
    bool verbose
) -> pup::Result<StaleCleanup>
{
    auto deleted = pup::Vec<pup::StringId> {};
    auto retired_commands = false;
    for (auto const& cmd : idx.commands()) {
        // Only delete outputs of a command whose directory we have authoritative
        // knowledge of this run; anything else is preserved.
        if (!is_dir_authoritative(idx, cmd.dir_id, parse_scopes, excludes, parsed_dirs, available_dirs, pruned_dirs)) {
            continue;
        }

        // Staleness is per file, not per command: a rule that drops one of its outputs
        // still joins through the ones it kept, so asking only "did this command survive"
        // would leave the dropped file owned by nothing and never delete it.
        for (auto const* edge : idx.edges_from(cmd.id)) {
            auto const* file = idx.find_file_by_id(edge->to);
            if (!file || file->type != pup::NodeType::Generated) {
                continue;
            }
            if (has_live_producer(join, file->path)) {
                continue;
            }

            // Paths now include build root (e.g., "build/program")
            auto file_path_sv = pup::global_pool().get(file->path);
            auto abs_path = pup::global_pool().get(pup::path::join(source_root, file_path_sv));
            if (pup::platform::exists(abs_path)) {
                if (dry_run) {
                    vprint(variant_name, "Would remove stale: {}\n", file_path_sv);
                } else {
                    if (auto removed = pup::platform::remove_file(abs_path); !removed) {
                        auto err = pup::Buf {};
                        err.fmt("Unable to remove previous output file: {}", file_path_sv);
                        return pup::make_error<StaleCleanup>(removed.error().code, err.view());
                    }
                    deleted.push_back(file->path);
                    if (verbose) {
                        vprint(variant_name, "  Removed stale: {}\n", file_path_sv);
                    }
                }
            }
        }

        if (!find_joined_command(join, idx, cmd)) {
            retired_commands = true;
            if (verbose) {
                // Not graph::command_label: the command has left the graph, but the index keeps its operands, so a pattern shared by a foreach still names one command.
                auto label = pup::is_empty(cmd.display) ? pup::index::get_command_string(idx, cmd) : cmd.display;
                auto const* dir = idx.find_file_by_id(cmd.dir_id);
                auto dir_sv = dir ? pup::global_pool().get(dir->path) : std::string_view {};
                auto verb = dry_run ? std::string_view { "Would remove command" } : std::string_view { "Removed command" };
                if (dir_sv.empty()) {
                    vprint(variant_name, "  {}: {}\n", verb, pup::global_pool().get(label));
                } else {
                    vprint(variant_name, "  {}: {} (in {})\n", verb, pup::global_pool().get(label), dir_sv);
                }
            }
        }
    }
    std::sort(deleted.begin(), deleted.end(), pup::handle_less);
    return StaleCleanup { .deleted = std::move(deleted), .retired_commands = retired_commands };
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
    auto excludes = make_exclude_list(opts);

    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
        .keep_going = opts.keep_going,
        .auto_init = true,
        .dry_run = opts.dry_run,
        .root_config_only = false,
        .require_config = true,
        .parse_scopes = parse_scopes,
        .excludes = excludes,
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
    // No early exit on an empty graph: having nothing to run is not having nothing to clean up (#231).
    auto num_commands = std::size_t { pup::graph::nodes_of_type(bs.graph, pup::NodeType::Command).size() };

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

    if (auto shadowed = reject_shadowed_sources(bs, old_idx_ptr, source_root_str, config_root_str); !shadowed) {
        veprint(variant_name, "Error: {}\n", shadowed.error().msg());
        return EXIT_FAILURE;
    }

    auto const all_deps_would_help = !opts.targets.empty() && !opts.include_all_deps;
    if (auto ghosts = reject_unresolved_ghosts(bs, pool.get(ctx.layout().output_root), all_deps_would_help); !ghosts) {
        veprint(variant_name, "Build failed: {}\n", ghosts.error().msg());
        return EXIT_FAILURE;
    }
    auto use_incremental = false;
    // Carries "has not succeeded since it last failed": seeded from the previous index, cleared
    // only by a successful run. A build in which the command does not run at all -- a target or
    // scoped build -- must not forget it.
    auto must_rerun_cmds = pup::NodeIdMap32 {};
    auto changed_files = pup::Vec<StringId> {};
    auto forced_cmds = pup::Vec<pup::NodeId> {};
    auto deleted_stale = pup::Vec<pup::StringId> {};

    if (old_idx_ptr) {
        auto const& idx = *old_idx_ptr;

        // Build the identity → NodeId map: the cross-build join key for commands.
        // Must happen after parsing (operands set) but before incremental logic.
        auto cmd_index_start = pup::SteadyClock::now();
        auto const join = graph_command_lookup(bs);
        auto cmd_index_elapsed = pup::SteadyClock::now() - cmd_index_start;
        pup::thread_metrics().command_index_time = std::chrono::duration_cast<std::chrono::microseconds>(cmd_index_elapsed);

        auto upstream_files = Vec<std::string_view> {};
        if (opts.include_all_deps && !scopes.empty()) {
            upstream_files = pup::graph::collect_upstream_files(bs, scopes);
        }

        // Always include implicit deps (headers from .d files) for in-scope
        // commands, even if the headers live outside the scoped directories.
        auto scope_crossing_inputs = Vec<StringId> {};
        if (!scopes.empty()) {
            scope_crossing_inputs = collect_scope_crossing_inputs(idx, scopes);
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
                if (!scope_crossing_inputs.empty()) {
                    print(" (+{} tracked inputs)", scope_crossing_inputs.size());
                }
                print("\n");
            }
        }

        // Before detection, not after: a deleted output is a change like any other, and
        // a consumer reaching it order-only is only notified if detection sees it gone.
        auto stale_start = pup::SteadyClock::now();
        auto stale_result = remove_stale_outputs(
            idx,
            join,
            parse_scopes,
            excludes,
            ctx.parsed_dirs(),
            ctx.available_dirs(),
            ctx.pruned_dirs(),
            source_root_str,
            variant_name,
            opts.dry_run,
            opts.verbose
        );
        if (!stale_result) {
            veprint(variant_name, "Build failed: {}\n", stale_result.error().msg());
            return EXIT_FAILURE;
        }
        auto const retired_commands = stale_result->retired_commands;
        deleted_stale = std::move(stale_result->deleted);
        auto stale_elapsed = pup::SteadyClock::now() - stale_start;
        pup::thread_metrics().stale_outputs_time = std::chrono::duration_cast<std::chrono::microseconds>(stale_elapsed);

        auto change_detect_start = pup::SteadyClock::now();
        changed_files = find_changed_files_with_implicit(
            source_root_str,
            config_root_str,
            pup::graph::get_build_root_name(bs.graph),
            idx,
            scopes,
            excludes,
            upstream_files,
            scope_crossing_inputs,
            collect_inactive_output_paths(bs),
            opts.verbose,
            opts.no_stat_cache
        );
        auto change_detect_elapsed = pup::SteadyClock::now() - change_detect_start;
        pup::thread_metrics().change_detection_time = std::chrono::duration_cast<std::chrono::microseconds>(change_detect_elapsed);

        auto input_delta = reconcile_input_set(bs, idx, changed_files, join, variant_name, opts.verbose);
        for (auto path_id : input_delta.changed_paths) {
            changed_files.push_back(path_id);
        }

        auto implicit_deps_start = pup::SteadyClock::now();
        auto implicit_delta = expand_implicit_deps(changed_files, idx, bs, join);
        changed_files = std::move(implicit_delta.changed_paths);
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
        for (auto cmd_id : input_delta.forced_cmds) {
            forced_cmds.push_back(cmd_id);
        }
        for (auto cmd_id : implicit_delta.forced_cmds) {
            forced_cmds.push_back(cmd_id);
        }
        for (auto cmd_id : new_cmds.must_rerun) {
            must_rerun_cmds.set(cmd_id, 1);
        }

        if (opts.rerun) {
            for (auto id : pup::graph::all_nodes(bs.graph)) {
                if (!pup::node_id::is_command(id) || !pup::graph::is_guard_satisfied(bs.graph, id)) {
                    continue;
                }
                if (!scopes.empty()) {
                    auto dir_sv = pup::global_pool().get(pup::graph::get<pup::graph::SourceDir>(bs.graph, id));
                    if (!pup::is_path_in_any_scope(dir_sv.empty() ? "." : dir_sv, scopes)) {
                        continue;
                    }
                }
                forced_cmds.push_back(id);
            }
        }

        // A retired record is not "nothing to do": only the write below persists the retirement (#245).
        if (changed_files.empty() && forced_cmds.empty() && !retired_commands) {
            vprint(variant_name, "Nothing to do (up to date).\n");
            if (opts.stat) {
                print_stats(idx, index_path, num_commands, 0, variant_start);
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
    auto executed_cmds = pup::Vec<pup::NodeId> {};

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
        if (job_result.success) {
            must_rerun_cmds.remove(job.id);
        }
        if (!job_result.success) {
            must_rerun_cmds.set(job.id, 1);
            if (use_tty_progress) {
                pup::exec::finalize_progress(prev_lines);
            }
            // The command, not the display: "CC main.o" does not say which flags broke, and this is the only line a build prints of what actually ran.
            veprint(variant_name, "FAILED: {}\n", pool.get(job.command));
            if (!pup::is_empty(job_result.output)) {
                auto output_sv = pool.get(job_result.output);
                eprint("{}\n", output_sv);
            }
        }

        auto target_id = job_result.deps_for_command != pup::INVALID_NODE_ID
            ? job_result.deps_for_command
            : job.id;
        if (job_result.success) {
            // Recorded even when it discovered nothing: an empty report is a report, and treating it as silence carried dead edges forever (#224).
            executed_cmds.push_back(target_id);
        }

        if (!job_result.discovered_deps.empty()) {
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
        // Nothing here will ever run a config rule, so a needing-to-run record on one is undischargeable.
        must_rerun_cmds.remove(cfg.cmd_id);
    }

    auto start = pup::SteadyClock::time_point { pup::SteadyClock::now() };

    // Composable filter: layer independent concerns, intersect when combined
    auto filter = BuildFilter {};

    if (use_incremental) {
        filter.intersect_with(pup::graph::collect_affected_commands(bs.graph, changed_files, forced_cmds));
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
        // Ahead of the dry-run branch so that a scheduler which ever fails a dry job reports it.
        vprint(variant_name, "Build completed: {} commands ({} failed) in {}ms\n", stats.completed_jobs, stats.failed_jobs, duration.count());
    } else if (opts.dry_run) {
        // "Would run", matching clean's "Would remove": a dry run has completed nothing.
        vprint(variant_name, "Would run: {} commands\n", stats.completed_jobs);
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
            old_idx_ptr,
            parse_scopes,
            excludes,
            ctx.parsed_dirs(),
            ctx.available_dirs(),
            ctx.pruned_dirs(),
            must_rerun_cmds,
            executed_cmds,
            deleted_stale
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
            print_stats(*final_index, index_path, num_commands, stats.completed_jobs, variant_start);
        } else if (old_idx_ptr) {
            print_stats(*old_idx_ptr, index_path, num_commands, stats.completed_jobs, variant_start);
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
