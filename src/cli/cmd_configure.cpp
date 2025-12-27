// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/config_commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/multi_variant.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/exec/scheduler.hpp"
#include "pup/graph/dag.hpp"

#include <cstdlib>
#include <fstream>
#include <set>

#include <fmt/core.h>

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
        fmt::print(stderr, "[{}] Error: {}\n", variant_name, result.error().message);
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
            fmt::print("[{}] Created {}\n", variant_name, config_path.string());
        }
    };

    auto configs = find_config_commands(ctx.graph(), ctx.layout().source_root);
    if (configs.empty()) {
        fmt::print("[{}] No config-generating rules found.\n", variant_name);
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
            fmt::print("[{}] Config rule: {} -> {}\n", variant_name, node ? node->display : "<unknown>", cfg.output_path);
        }
    }

    if (config_commands.empty()) {
        fmt::print("[{}] No config-generating rules in scope.\n", variant_name);
        ensure_config();
        return EXIT_SUCCESS;
    }

    auto all_commands = collect_command_dependencies(ctx.graph(), config_commands);
    auto dep_count = all_commands.size() - config_commands.size();
    if (dep_count > 0 && opts.verbose) {
        fmt::print("[{}] Config rules depend on {} additional command(s)\n", variant_name, dep_count);
    }
    fmt::print("[{}] Found {} config-generating rule(s)\n", variant_name, config_commands.size());

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
            fmt::print("[{}] {}\n", variant_name, job.display);
        }
    });

    scheduler.on_job_complete([&](pup::exec::BuildJob const& job, pup::exec::JobResult const& job_result) {
        if (!job_result.success) {
            fmt::print(stderr, "[{}] FAILED: {}\n", variant_name, job.display);
            if (!job_result.output.empty()) {
                fmt::print(stderr, "[{}] {}\n", variant_name, job_result.output);
            }
        }
    });

    scheduler.on_progress([&](std::size_t done, std::size_t total) {
        if (!opts.verbose) {
            fmt::print("[{}] [{}/{}] ", variant_name, done, total);
            std::fflush(stdout);
        }
    });

    auto build_result = scheduler.build_subset(ctx.graph(), all_commands);

    if (!opts.verbose) {
        fmt::print("\n");
    }

    if (!build_result) {
        fmt::print(stderr, "[{}] Configure failed: {}\n", variant_name, build_result.error().message);
        return EXIT_FAILURE;
    }

    auto const& stats = *build_result;
    if (stats.failed_jobs > 0) {
        fmt::print("[{}] Configure completed: {} commands ({} failed)\n", variant_name, stats.completed_jobs, stats.failed_jobs);
        return EXIT_FAILURE;
    }

    fmt::print("[{}] Configure completed: {} commands\n", variant_name, stats.completed_jobs);
    ensure_config();
    return EXIT_SUCCESS;
}

} // namespace

auto cmd_configure(Options const& opts) -> int
{
    return for_each_variant(opts, configure_single_variant, "Configuring");
}

} // namespace pup::cli
