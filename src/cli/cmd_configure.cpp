// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/config_commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/multi_variant.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/exec/scheduler.hpp"
#include "pup/graph/dag.hpp"
#include "pup/platform/file_io.hpp"

#include <cstdio>
#include <cstdlib>

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
        fprintf(stderr, "[%.*s] Error: Config file not found: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), cp.c_str());
        return EXIT_FAILURE;
    }

    auto dest_sv = pool.get(pup::path::join(pool.get(layout.output_root), "tup.config"));
    (void)pup::platform::create_directories(pup::path::parent(dest_sv));
    (void)pup::platform::copy_file(config_path_sv, dest_sv);

    auto cp = Buf {};
    cp.append(config_path_sv);
    auto dp = Buf {};
    dp.append(dest_sv);
    printf("[%.*s] Installed %s -> %s\n", static_cast<int>(variant_name.size()), variant_name.data(), cp.c_str(), dp.c_str());
    return EXIT_SUCCESS;
}

auto configure_single_variant(
    Options const& opts,
    std::string_view variant_name
) -> int
{
    auto layout = discover_layout(make_layout_options(opts));
    if (!layout) {
        fprintf(stderr, "[%.*s] Error: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), layout.error().msg().data());
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
        fprintf(stderr, "[%.*s] Error: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), result.error().msg().data());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;

    // Helper to ensure tup.config exists for variant detection (only on success)
    auto ensure_config = [&]() {
        auto config_path_sv = pool.get(pup::path::join(pool.get(ctx.layout().output_root), "tup.config"));
        if (!pup::platform::exists(config_path_sv)) {
            (void)pup::platform::create_directories(pup::path::parent(config_path_sv));
            (void)pup::platform::write_file(config_path_sv, "");
            auto cp = Buf {};
            cp.append(config_path_sv);
            printf("[%.*s] Created %s\n", static_cast<int>(variant_name.size()), variant_name.data(), cp.c_str());
        }
    };

    auto configs = find_config_commands(ctx.graph(), pool.get(ctx.layout().source_root));
    if (configs.empty()) {
        printf("[%.*s] No config-generating rules found.\n", static_cast<int>(variant_name.size()), variant_name.data());
        ensure_config();
        return EXIT_SUCCESS;
    }

    // Filter config commands by scope if specified
    auto config_commands = pup::NodeIdMap32 {};
    for (auto const& cfg : configs) {
        auto const* node = ctx.graph().get_command_node(cfg.cmd_id);
        auto source_dir_sv = node ? pup::graph::get_source_dir(ctx.graph().graph(), cfg.cmd_id) : std::string_view {};
        if (!scopes.empty() && node && !pup::is_path_in_any_scope(source_dir_sv, scopes)) {
            continue;
        }
        config_commands.set(cfg.cmd_id, 1);
        if (opts.verbose) {
            auto display_sv = node ? pup::graph::get_display_str(ctx.graph().graph(), cfg.cmd_id) : std::string_view { "<unknown>" };
            auto output_path_sv = pool.get(cfg.output_path);
            printf("[%.*s] Config rule: %.*s -> %.*s\n", static_cast<int>(variant_name.size()), variant_name.data(), static_cast<int>(display_sv.size()), display_sv.data(), static_cast<int>(output_path_sv.size()), output_path_sv.data());
        }
    }

    if (config_commands.empty()) {
        printf("[%.*s] No config-generating rules in scope.\n", static_cast<int>(variant_name.size()), variant_name.data());
        ensure_config();
        return EXIT_SUCCESS;
    }

    auto all_commands = collect_command_dependencies(ctx.graph(), config_commands);
    auto dep_count = all_commands.size() - config_commands.size();
    if (dep_count > 0 && opts.verbose) {
        printf("[%.*s] Config rules depend on %zu additional command(s)\n", static_cast<int>(variant_name.size()), variant_name.data(), dep_count);
    }
    printf("[%.*s] Found %zu config-generating rule(s)\n", static_cast<int>(variant_name.size()), variant_name.data(), config_commands.size());

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
            printf("[%.*s] %.*s\n", static_cast<int>(variant_name.size()), variant_name.data(), static_cast<int>(display_sv.size()), display_sv.data());
        }
    });

    scheduler.on_job_complete([&](pup::exec::BuildJob const& job, pup::exec::JobResult const& job_result) {
        if (!job_result.success) {
            auto display_sv = pool.get(job.display);
            fprintf(stderr, "[%.*s] FAILED: %.*s\n", static_cast<int>(variant_name.size()), variant_name.data(), static_cast<int>(display_sv.size()), display_sv.data());
            if (!pup::is_empty(job_result.output)) {
                auto output_sv = pool.get(job_result.output);
                fprintf(stderr, "[%.*s] %.*s\n", static_cast<int>(variant_name.size()), variant_name.data(), static_cast<int>(output_sv.size()), output_sv.data());
            }
        }
    });

    scheduler.on_progress([&](std::size_t done, std::size_t total) {
        if (!opts.verbose) {
            printf("\r[%.*s] [%zu/%zu] ", static_cast<int>(variant_name.size()), variant_name.data(), done, total);
            std::fflush(stdout);
        }
    });

    auto build_result = scheduler.build_subset(ctx.graph(), all_commands);

    if (!opts.verbose) {
        printf("\n");
    }

    if (!build_result) {
        fprintf(stderr, "[%.*s] Configure failed: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), build_result.error().msg().data());
        return EXIT_FAILURE;
    }

    auto const& stats = *build_result;
    if (stats.failed_jobs > 0) {
        printf("[%.*s] Configure completed: %zu commands (%zu failed)\n", static_cast<int>(variant_name.size()), variant_name.data(), stats.completed_jobs, stats.failed_jobs);
        return EXIT_FAILURE;
    }

    printf("[%.*s] Configure completed: %zu commands\n", static_cast<int>(variant_name.size()), variant_name.data(), stats.completed_jobs);
    ensure_config();
    return EXIT_SUCCESS;
}

} // namespace

auto cmd_configure(Options const& opts) -> int
{
    return for_each_variant(opts, configure_single_variant, "Configuring");
}

} // namespace pup::cli
