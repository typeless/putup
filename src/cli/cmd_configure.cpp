// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/config_commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/multi_variant.hpp"
#include "pup/cli/options.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/node_id_map.hpp"
#include "pup/core/path.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/core/print.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/exec/scheduler.hpp"
#include "pup/graph/dag.hpp"
#include "pup/platform/file_io.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace pup::cli {

namespace {

auto install_config_file(
    ProjectLayout const& layout,
    std::string_view config_file,
    std::string_view variant_name
) -> int
{
    auto& pool = pup::global_pool();
    auto config_path_sv = pup::path::is_absolute(config_file)
        ? config_file
        : pool.get(pup::path::join(pool.get(*pup::platform::current_directory()), config_file));

    if (!pup::platform::exists(config_path_sv)) {
        auto cp = Buf {};
        cp.append(config_path_sv);
        eprint("[{}] Error: Config file not found: {}\n", variant_name, cp.c_str());
        return EXIT_FAILURE;
    }

    auto dest_sv = pool.get(pup::path::join(pool.get(layout.output_root), "tup.config"));
    auto cp = Buf {};
    cp.append(config_path_sv);
    auto dp = Buf {};
    dp.append(dest_sv);

    if (auto r = pup::platform::create_directories(pup::path::parent(dest_sv)); !r) {
        eprint("[{}] Error: could not install {} -> {}: {}\n", variant_name, cp.c_str(), dp.c_str(), r.error().msg());
        return EXIT_FAILURE;
    }
    if (auto r = pup::platform::copy_file(config_path_sv, dest_sv); !r) {
        eprint("[{}] Error: could not install {} -> {}: {}\n", variant_name, cp.c_str(), dp.c_str(), r.error().msg());
        return EXIT_FAILURE;
    }

    print("[{}] Installed {} -> {}\n", variant_name, cp.c_str(), dp.c_str());
    return EXIT_SUCCESS;
}

auto configure_single_variant(
    Options const& opts,
    std::string_view variant_name
) -> int
{
    auto layout = discover_layout(make_layout_options(opts));
    if (!layout) {
        eprint("[{}] Error: {}\n", variant_name, layout.error().msg());
        return EXIT_FAILURE;
    }

    auto& pool = pup::global_pool();

    // Step 1: Install root config if --config specified
    if (!pup::is_empty(opts.config_file)) {
        auto rc = install_config_file(*layout, pool.get(opts.config_file), variant_name);
        if (rc != EXIT_SUCCESS) {
            return rc;
        }
    }

    // Step 2: Run config-generating rules
    auto scopes = compute_build_scopes(opts, *layout);
    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
        .keep_going = opts.keep_going,
        .auto_init = false,       // Configure pass should not create .pup
        .root_config_only = true, // Configure uses only root tup.config
        .parse_scopes = scopes,
        .scanner_registry = nullptr,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        eprint("[{}] Error: {}\n", variant_name, result.error().msg());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;

    // Helper to ensure tup.config exists for variant detection (only on success)
    auto ensure_config = [&]() -> int {
        auto config_path_sv = pool.get(pup::path::join(pool.get(ctx.layout().output_root), "tup.config"));
        if (pup::platform::exists(config_path_sv)) {
            return EXIT_SUCCESS;
        }
        auto cp = Buf {};
        cp.append(config_path_sv);
        // Guarded here rather than at the three call sites: a dry run that writes tup.config
        // disarms the "run configure first" gate, so a later plain build silently proceeds.
        if (opts.dry_run) {
            print("[{}] Would create {}\n", variant_name, cp.c_str());
            return EXIT_SUCCESS;
        }
        if (auto r = pup::platform::create_directories(pup::path::parent(config_path_sv)); !r) {
            eprint("[{}] Error: could not create {}: {}\n", variant_name, cp.c_str(), r.error().msg());
            return EXIT_FAILURE;
        }
        if (auto r = pup::platform::write_file(config_path_sv, ""); !r) {
            eprint("[{}] Error: could not create {}: {}\n", variant_name, cp.c_str(), r.error().msg());
            return EXIT_FAILURE;
        }
        print("[{}] Created {}\n", variant_name, cp.c_str());
        return EXIT_SUCCESS;
    };

    auto configs = find_config_commands(ctx.graph(), pool.get(ctx.layout().source_root));
    if (configs.empty()) {
        print("[{}] No config-generating rules found.\n", variant_name);
        return ensure_config();
    }

    // Filter config commands by scope if specified
    auto config_commands = pup::NodeIdMap32 {};
    auto label_cache = pup::graph::PathCache {};
    for (auto const& cfg : configs) {
        auto source_dir_sv = pool.get(pup::graph::get<pup::graph::SourceDir>(ctx.graph().graph, cfg.cmd_id));
        if (!scopes.empty() && !pup::is_path_in_any_scope(source_dir_sv, scopes)) {
            continue;
        }
        config_commands.set(cfg.cmd_id, 1);
        if (opts.verbose) {
            auto display_sv = pool.get(
                pup::graph::command_label(ctx.graph().graph, cfg.cmd_id, label_cache, pool.get(ctx.layout().source_root), pool.get(ctx.layout().config_root))
            );
            auto output_path_sv = pool.get(cfg.output_path);
            print("[{}] Config rule: {} -> {}\n", variant_name, display_sv, output_path_sv);
        }
    }

    if (config_commands.empty()) {
        print("[{}] No config-generating rules in scope.\n", variant_name);
        return ensure_config();
    }

    auto all_commands = collect_command_dependencies(ctx.graph(), config_commands);
    auto dep_count = all_commands.size() - config_commands.size();
    if (dep_count > 0 && opts.verbose) {
        print("[{}] Config rules depend on {} additional command(s)\n", variant_name, dep_count);
    }
    print("[{}] Found {} config-generating rule(s)\n", variant_name, config_commands.size());

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

    scheduler.on_job_start([&](pup::exec::BuildJob const& job) {
        if (opts.verbose || opts.dry_run) {
            auto display_sv = pool.get(job.display);
            print("[{}] {}\n", variant_name, display_sv);
        }
    });

    scheduler.on_job_complete([&](pup::exec::BuildJob const& job, pup::exec::JobResult const& job_result) {
        if (!job_result.success) {
            // The command, not the display: configure's own echo shows the annotation, so this is the only line naming what actually ran.
            eprint("[{}] FAILED: {}\n", variant_name, pool.get(job.command));
            if (!pup::is_empty(job_result.output)) {
                auto output_sv = pool.get(job_result.output);
                eprint("[{}] {}\n", variant_name, output_sv);
            }
        }
    });

    scheduler.on_progress([&](std::size_t done, std::size_t total) {
        if (!opts.verbose) {
            print("\r[{}] [{}/{}] ", variant_name, done, total);
            flush(Stream::Out);
        }
    });

    auto build_result = scheduler.build(ctx.graph(), &all_commands);

    if (!opts.verbose) {
        print("\n");
    }

    if (!build_result) {
        eprint("[{}] Configure failed: {}\n", variant_name, build_result.error().msg());
        return EXIT_FAILURE;
    }

    auto const& stats = *build_result;
    if (stats.failed_jobs > 0) {
        print("[{}] Configure completed: {} commands ({} failed)\n", variant_name, stats.completed_jobs, stats.failed_jobs);
        return EXIT_FAILURE;
    }

    if (opts.dry_run) {
        print("[{}] Would run: {} commands\n", variant_name, stats.completed_jobs);
    } else {
        print("[{}] Configure completed: {} commands\n", variant_name, stats.completed_jobs);
    }
    return ensure_config();
}

} // namespace

auto cmd_configure(Options const& opts) -> int
{
    return for_each_variant(opts, configure_single_variant, "Configuring");
}

} // namespace pup::cli
