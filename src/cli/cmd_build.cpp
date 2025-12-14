// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/core/hash.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/types.hpp"
#include "pup/exec/scheduler.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/rule_pattern.hpp"
#include "pup/index/entry.hpp"
#include "pup/index/reader.hpp"
#include "pup/index/writer.hpp"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <unordered_map>

#include <fmt/core.h>

namespace pup::cli {

namespace {

auto get_file_mtime(std::filesystem::path const& path) -> pup::FileTime
{
    struct stat st = {};
    if (::stat(path.c_str(), &st) < 0) {
        return {};
    }
    return pup::FileTime {
        .seconds = st.st_mtim.tv_sec,
        .nanoseconds = static_cast<std::int32_t>(st.st_mtim.tv_nsec),
    };
}

auto resolve_path(
    std::filesystem::path const& path,
    std::filesystem::path const& root) -> std::filesystem::path
{
    return path.is_absolute() ? path : root / path;
}

auto is_path_under_root(
    std::filesystem::path const& path,
    std::filesystem::path const& root) -> bool
{
    auto path_str = path.string();
    auto root_str = root.string();
    if (!root_str.empty() && root_str.back() != '/') {
        root_str += '/';
    }
    return path_str.starts_with(root_str) || path == root;
}

auto find_changed_files_with_implicit(
    std::filesystem::path const& root,
    pup::index::Index const& old_index,
    bool verbose = false) -> std::vector<std::string>
{
    auto changed = std::vector<std::string> {};

    for (auto const& file : old_index.files()) {
        if (file.type != pup::NodeType::File && file.type != pup::NodeType::Generated) {
            continue;
        }

        auto path = resolve_path(file.path, root);

        struct stat st = {};
        if (::stat(path.c_str(), &st) < 0) {
            if (verbose) {
                fmt::print("  Changed (stat failed): {}\n", file.path);
            }
            changed.push_back(file.path);
            continue;
        }

        auto current_mtime = pup::FileTime {
            .seconds = st.st_mtim.tv_sec,
            .nanoseconds = static_cast<std::int32_t>(st.st_mtim.tv_nsec),
        };

        if (current_mtime != file.mtime) {
            if (verbose) {
                fmt::print("  Changed (mtime): {} - stored {}:{} vs current {}:{}\n",
                    file.path, file.mtime.seconds, file.mtime.nanoseconds,
                    current_mtime.seconds, current_mtime.nanoseconds);
            }
            changed.push_back(file.path);
            continue;
        }

        auto current_size = static_cast<std::uint64_t>(st.st_size);
        if (current_size != file.size) {
            if (verbose) {
                fmt::print("  Changed (size): {}\n", file.path);
            }
            changed.push_back(file.path);
            continue;
        }

        if (file.content_hash != pup::ZERO_HASH) {
            auto hash_result = pup::sha256_file(path);
            if (!hash_result || *hash_result != file.content_hash) {
                if (verbose) {
                    fmt::print("  Changed (hash): {}\n", file.path);
                }
                changed.push_back(file.path);
            }
        }
    }

    return changed;
}

auto expand_implicit_deps(
    std::vector<std::string> const& changed,
    pup::index::Index const& index,
    pup::graph::BuildGraph const& graph) -> std::vector<std::string>
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
    std::filesystem::path const& root,
    pup::index::Index const* old_index = nullptr) -> pup::index::Index
{
    auto index = pup::index::Index {};
    auto path_to_id = std::unordered_map<std::string, pup::NodeId> {};

    auto max_id = pup::NodeId { 0 };
    for (auto const& node : graph) {
        if (node.id > max_id) {
            max_id = node.id;
        }

        if (node.type == pup::NodeType::File || node.type == pup::NodeType::Generated) {
            auto node_path = graph.get_full_path(node.id);
            if (node_path.empty()) {
                continue;
            }

            auto file_path = std::filesystem::path { root / node_path };
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
                .id = node.id,
                .parent_id = node.parent_dir,
                .src_id = 0,
                .type = node.type,
                .flags = node.flags,
                .name = node.name,
                .path = node_path,
                .size = file_size,
                .mtime = get_file_mtime(file_path),
                .content_hash = content_hash,
            };
            index.add_file(std::move(entry));
            path_to_id[node_path] = node.id;
        } else if (node.type == pup::NodeType::Directory || node.type == pup::NodeType::GeneratedDir) {
            auto node_path = graph.get_full_path(node.id);
            auto entry = pup::index::FileEntry {
                .id = node.id,
                .parent_id = node.parent_dir,
                .src_id = 0,
                .type = node.type,
                .flags = node.flags,
                .name = node.name,
                .path = node_path,
                .size = 0,
                .mtime = {},
                .content_hash = {},
            };
            index.add_file(std::move(entry));
            if (!node_path.empty()) {
                path_to_id[node_path] = node.id;
            }
        } else if (node.type == pup::NodeType::Command) {
            auto entry = pup::index::CommandEntry {
                .id = node.id,
                .dir_id = node.parent_dir,
                .command = node.command,
                .display = node.display,
                .env = {},
                .flags = 0,
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

    auto next_id = pup::NodeId { max_id + 1 };
    auto added_edges = std::set<std::pair<pup::NodeId, pup::NodeId>> {};

    auto get_or_create_dir = [&](this auto& self, std::filesystem::path const& dir_path) -> pup::NodeId {
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
                .mtime = {},
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
            .mtime = {},
            .content_hash = {},
        };
        index.add_file(std::move(entry));
        path_to_id[path_str] = dir_id;
        return dir_id;
    };

    for (auto const& [cmd_id, deps] : discovered_deps) {
        for (auto const& dep_path : deps) {
            auto abs_path = resolve_path(dep_path, root);

            auto rel_path = std::string {};
            if (is_path_under_root(abs_path, root)) {
                rel_path = std::filesystem::relative(abs_path, root).string();
            } else {
                rel_path = abs_path.string();
            }

            auto dep_id = pup::NodeId { 0 };
            auto it = path_to_id.find(rel_path);
            if (it != path_to_id.end()) {
                dep_id = it->second;
            } else {
                dep_id = next_id++;

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

                auto fs_path = std::filesystem::path { rel_path };
                auto parent_id = get_or_create_dir(fs_path.parent_path());
                auto basename = fs_path.filename().string();

                auto entry = pup::index::FileEntry {
                    .id = dep_id,
                    .parent_id = parent_id,
                    .src_id = 0,
                    .type = pup::NodeType::File,
                    .flags = pup::NodeFlags::None,
                    .name = basename,
                    .path = rel_path,
                    .size = file_size,
                    .mtime = get_file_mtime(abs_path),
                    .content_hash = content_hash,
                };
                index.add_file(std::move(entry));
                path_to_id[rel_path] = dep_id;
            }

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
            pup::NodeId new_from_id;
            if (new_file_it != path_to_id.end()) {
                new_from_id = new_file_it->second;
            } else {
                new_from_id = next_id++;

                auto abs_path = resolve_path(old_file->path, root);
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

                auto fs_path = std::filesystem::path { old_file->path };
                auto parent_id = get_or_create_dir(fs_path.parent_path());
                auto basename = fs_path.filename().string();

                auto entry = pup::index::FileEntry {
                    .id = new_from_id,
                    .parent_id = parent_id,
                    .src_id = 0,
                    .type = pup::NodeType::File,
                    .flags = pup::NodeFlags::None,
                    .name = basename,
                    .path = old_file->path,
                    .size = file_size,
                    .mtime = get_file_mtime(abs_path),
                    .content_hash = content_hash,
                };
                index.add_file(std::move(entry));
                path_to_id[old_file->path] = new_from_id;
            }

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

    return index;
}

} // namespace

auto cmd_build(Options const& opts) -> int
{
    auto pattern_registry = std::optional<pup::graph::RulePatternRegistry> {};
    auto implicit_deps_disabled = false;
    if (auto const* env = std::getenv("PUP_IMPLICIT_DEPS"); env && std::string_view { env } == "0") {
        implicit_deps_disabled = true;
    }

    if (!implicit_deps_disabled) {
        pattern_registry.emplace();
        pattern_registry->register_pattern(pup::graph::make_gcc_depfile_pattern());
        if (opts.verbose) {
            fmt::print("Implicit dependency tracking enabled\n");
        }
    }

    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
        .keep_going = opts.keep_going,
        .auto_init = true,
        .pattern_registry = pattern_registry ? &*pattern_registry : nullptr,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        fmt::print(stderr, "Error: {}\n", result.error().message);
        return EXIT_FAILURE;
    }

    auto& ctx = *result;
    auto num_commands = std::size_t { ctx.graph().nodes_of_type(pup::NodeType::Command).size() };

    if (num_commands == 0) {
        fmt::print("Nothing to do.\n");
        return EXIT_SUCCESS;
    }

    auto index_path = ctx.layout().index_path();
    auto old_index = std::optional<pup::index::Index> {};
    auto use_incremental = false;
    auto changed_files = std::vector<std::string> {};

    if (std::filesystem::exists(index_path)) {
        auto reader_result = pup::Result<pup::index::IndexReader> { pup::index::IndexReader::open(index_path) };
        if (reader_result) {
            auto index_result = pup::Result<pup::index::Index> { reader_result->read() };
            if (index_result) {
                old_index = std::move(*index_result);
                changed_files = find_changed_files_with_implicit(ctx.layout().source_root, *old_index, opts.verbose);
                changed_files = expand_implicit_deps(changed_files, *old_index, ctx.graph());

                if (changed_files.empty()) {
                    fmt::print("Nothing to do (up to date).\n");
                    return EXIT_SUCCESS;
                }

                use_incremental = true;
                if (opts.verbose) {
                    fmt::print("Incremental build: {} changed files\n", changed_files.size());
                }
            }
        }
    }

    auto sched_opts = pup::exec::SchedulerOptions {
        .jobs = opts.jobs,
        .keep_going = opts.keep_going,
        .dry_run = opts.dry_run,
        .verbose = opts.verbose,
        .source_root = ctx.layout().source_root,
        .output_root = ctx.layout().output_root,
        .variant_dir = ctx.layout().variant_dir,
    };

    auto scheduler = pup::exec::Scheduler { sched_opts };
    auto discovered_deps = std::unordered_map<pup::NodeId, std::vector<std::string>> {};
    auto deps_mutex = std::mutex {};

    scheduler.on_job_start([&](pup::exec::BuildJob const& job) {
        if (opts.verbose || opts.dry_run) {
            fmt::print("{}\n", job.display);
        }
    });

    scheduler.on_job_complete([&](pup::exec::BuildJob const& job, pup::exec::JobResult const& job_result) {
        if (!job_result.success) {
            fmt::print(stderr, "FAILED: {}\n", job.display);
            if (!job_result.output.empty()) {
                fmt::print(stderr, "{}\n", job_result.output);
            }
        } else if (!job_result.discovered_deps.empty()) {
            auto lock = std::lock_guard { deps_mutex };
            auto target_id = job_result.deps_for_command != pup::INVALID_NODE_ID
                ? job_result.deps_for_command
                : job.id;
            auto& deps = discovered_deps[target_id];

            for (auto const& dep_path : job_result.discovered_deps) {
                try {
                    auto resolved = std::filesystem::path {};
                    if (std::filesystem::path { dep_path }.is_absolute()) {
                        resolved = std::filesystem::weakly_canonical(dep_path);
                    } else {
                        resolved = std::filesystem::weakly_canonical(job.working_dir / dep_path);
                    }

                    if (is_path_under_root(resolved, ctx.layout().source_root)) {
                        deps.push_back(std::filesystem::relative(resolved, ctx.layout().source_root).string());
                    } else {
                        deps.push_back(resolved.string());
                    }
                } catch (std::filesystem::filesystem_error const& e) {
                    if (opts.verbose) {
                        fmt::print(stderr, "Warning: Skipping dependency '{}': {}\n", dep_path, e.what());
                    }
                }
            }
        }
    });

    auto completed = std::size_t { 0 };
    scheduler.on_progress([&](std::size_t done, std::size_t total) {
        completed = done;
        if (!opts.verbose) {
            fmt::print("\r[{}/{}] ", done, total);
            std::fflush(stdout);
        }
    });

    auto start = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
    auto build_result = pup::Result<pup::exec::BuildStats> {};
    if (use_incremental && old_index) {
        build_result = scheduler.build_incremental(ctx.graph(), *old_index, changed_files);
    } else {
        build_result = scheduler.build(ctx.graph());
    }
    auto end = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
    auto duration = std::chrono::milliseconds { std::chrono::duration_cast<std::chrono::milliseconds>(end - start) };

    if (!opts.verbose) {
        fmt::print("\n");
    }

    if (!build_result) {
        fmt::print(stderr, "Build failed: {}\n", build_result.error().message);
        return EXIT_FAILURE;
    }

    auto const& stats = *build_result;
    if (stats.failed_jobs > 0) {
        fmt::print("Build completed: {} commands ({} failed) in {}ms\n",
            stats.completed_jobs, stats.failed_jobs, duration.count());
    } else {
        fmt::print("Build completed: {} commands in {}ms\n",
            stats.completed_jobs, duration.count());
    }

    if (stats.failed_jobs == 0 && !opts.dry_run) {
        auto const* old_index_ptr = old_index ? &*old_index : nullptr;
        auto index = pup::index::Index { build_index(ctx.graph(), discovered_deps, ctx.layout().source_root, old_index_ptr) };
        auto writer = pup::index::IndexWriter {};
        auto write_result = pup::Result<void> { writer.write(index_path, index) };
        if (!write_result) {
            fmt::print(stderr, "Warning: Failed to save index: {}\n", write_result.error().message);
        } else if (opts.verbose) {
            fmt::print("Saved index: {} files, {} commands, {} edges\n",
                index.file_count(), index.command_count(), index.edge_count());
        }
    }

    return stats.failed_jobs > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

} // namespace pup::cli
