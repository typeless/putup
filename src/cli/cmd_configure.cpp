// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/config_commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/multi_variant.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/exec/scheduler.hpp"
#include "pup/graph/dag.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>

namespace pup::cli {

namespace {

auto configure_single_variant(
    Options const& opts,
    std::string_view variant_name
) -> int
{
    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
        .keep_going = opts.keep_going,
        .auto_init = false,       // Configure pass should not create .pup
        .root_config_only = true, // Configure uses only root tup.config
        .scanner_registry = nullptr,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        fprintf(stderr, "[%.*s] Error: %s\n",
            static_cast<int>(variant_name.size()), variant_name.data(),
            result.error().message.c_str());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;

    // Helper to ensure tup.config exists for variant detection (only on success)
    auto ensure_config = [&]() {
        auto config_path = ctx.layout().output_root / "tup.config";
        if (!std::filesystem::exists(config_path)) {
            std::filesystem::create_directories(config_path.parent_path());
            auto ofs = std::ofstream { config_path };
            ofs.close();
            printf("[%.*s] Created %s\n",
                static_cast<int>(variant_name.size()), variant_name.data(),
                config_path.string().c_str());
        }
    };

    auto configs = find_config_commands(ctx.graph(), ctx.layout().source_root);
    if (configs.empty()) {
        printf("[%.*s] No config-generating rules found.\n",
            static_cast<int>(variant_name.size()), variant_name.data());
        ensure_config();
        return EXIT_SUCCESS;
    }

    // Filter config commands by scope if specified
    auto scopes = compute_build_scopes(opts, ctx.layout());
    auto config_commands = std::set<pup::NodeId> {};
    for (auto const& cfg : configs) {
        auto const* node = ctx.graph().get_node(cfg.cmd_id);
        if (!scopes.empty() && node && !pup::is_path_in_any_scope(node->source_dir, scopes)) {
            continue;
        }
        config_commands.insert(cfg.cmd_id);
        if (opts.verbose) {
            printf("[%.*s] Config rule: %s -> %s\n",
                static_cast<int>(variant_name.size()), variant_name.data(),
                node ? node->display.c_str() : "<unknown>",
                cfg.output_path.c_str());
        }
    }

    if (config_commands.empty()) {
        printf("[%.*s] No config-generating rules in scope.\n",
            static_cast<int>(variant_name.size()), variant_name.data());
        ensure_config();
        return EXIT_SUCCESS;
    }

    auto all_commands = collect_command_dependencies(ctx.graph(), config_commands);
    auto dep_count = all_commands.size() - config_commands.size();
    if (dep_count > 0 && opts.verbose) {
        printf("[%.*s] Config rules depend on %zu additional command(s)\n",
            static_cast<int>(variant_name.size()), variant_name.data(),
            dep_count);
    }
    printf("[%.*s] Found %zu config-generating rule(s)\n",
        static_cast<int>(variant_name.size()), variant_name.data(),
        config_commands.size());

    auto sched_opts = pup::exec::SchedulerOptions {
        .jobs = opts.jobs,
        .keep_going = opts.keep_going,
        .dry_run = opts.dry_run,
        .verbose = opts.verbose,
        .source_root = ctx.layout().source_root,
        .output_root = ctx.layout().output_root,
    };

    auto scheduler = pup::exec::Scheduler { sched_opts };

    scheduler.on_job_start([&](pup::exec::BuildJob const& job) {
        if (opts.verbose || opts.dry_run) {
            printf("[%.*s] %s\n",
                static_cast<int>(variant_name.size()), variant_name.data(),
                job.display.c_str());
        }
    });

    scheduler.on_job_complete([&](pup::exec::BuildJob const& job, pup::exec::JobResult const& job_result) {
        if (!job_result.success) {
            fprintf(stderr, "[%.*s] FAILED: %s\n",
                static_cast<int>(variant_name.size()), variant_name.data(),
                job.display.c_str());
            if (!job_result.output.empty()) {
                fprintf(stderr, "[%.*s] %s\n",
                    static_cast<int>(variant_name.size()), variant_name.data(),
                    job_result.output.c_str());
            }
        }
    });

    scheduler.on_progress([&](std::size_t done, std::size_t total) {
        if (!opts.verbose) {
            printf("[%.*s] [%zu/%zu] ",
                static_cast<int>(variant_name.size()), variant_name.data(),
                done, total);
            std::fflush(stdout);
        }
    });

    auto build_result = scheduler.build_subset(ctx.graph(), all_commands);

    if (!opts.verbose) {
        printf("\n");
    }

    if (!build_result) {
        fprintf(stderr, "[%.*s] Configure failed: %s\n",
            static_cast<int>(variant_name.size()), variant_name.data(),
            build_result.error().message.c_str());
        return EXIT_FAILURE;
    }

    auto const& stats = *build_result;
    if (stats.failed_jobs > 0) {
        printf("[%.*s] Configure completed: %zu commands (%zu failed)\n",
            static_cast<int>(variant_name.size()), variant_name.data(),
            stats.completed_jobs, stats.failed_jobs);
        return EXIT_FAILURE;
    }

    printf("[%.*s] Configure completed: %zu commands\n",
        static_cast<int>(variant_name.size()), variant_name.data(),
        stats.completed_jobs);
    ensure_config();
    return EXIT_SUCCESS;
}

} // namespace

auto cmd_configure(Options const& opts) -> int
{
    return for_each_variant(opts, configure_single_variant, "Configuring");
}

} // namespace pup::cli
