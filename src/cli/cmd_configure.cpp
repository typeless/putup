// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/commands.hpp"
#include "pup/platform/file_io.hpp"
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

auto install_config_file(
    ProjectLayout const& layout,
    std::string const& config_file,
    std::string_view variant_name
) -> int
{
    auto config_path = std::string { config_file };
    if (!pup::path::is_absolute(config_path)) {
        config_path = pup::path::join(*pup::platform::current_directory(), config_path);
    }

    if (!pup::platform::exists(config_path)) {
        fprintf(stderr, "[%.*s] Error: Config file not found: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), config_path.c_str());
        return EXIT_FAILURE;
    }

    auto dest = pup::path::join(layout.output_root, "tup.config");
    (void)pup::platform::create_directories(std::string { pup::path::parent(dest) });
    (void)pup::platform::copy_file(config_path, dest);

    printf("[%.*s] Installed %s -> %s\n", static_cast<int>(variant_name.size()), variant_name.data(), config_path.c_str(), dest.c_str());
    return EXIT_SUCCESS;
}

auto configure_single_variant(
    Options const& opts,
    std::string_view variant_name
) -> int
{
    auto layout = discover_layout(make_layout_options(opts));
    if (!layout) {
        fprintf(stderr, "[%.*s] Error: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), layout.error().message.c_str());
        return EXIT_FAILURE;
    }

    // Step 1: Install root config if --config specified
    if (!opts.config_file.empty()) {
        auto rc = install_config_file(*layout, opts.config_file, variant_name);
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
        fprintf(stderr, "[%.*s] Error: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), result.error().message.c_str());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;

    // Helper to ensure tup.config exists for variant detection (only on success)
    auto ensure_config = [&]() {
        auto config_path = pup::path::join(ctx.layout().output_root, "tup.config");
        if (!pup::platform::exists(config_path)) {
            (void)pup::platform::create_directories(std::string { pup::path::parent(config_path) });
            auto ofs = std::ofstream { config_path };
            ofs.close();
            printf("[%.*s] Created %s\n", static_cast<int>(variant_name.size()), variant_name.data(), config_path.c_str());
        }
    };

    auto configs = find_config_commands(ctx.graph(), ctx.layout().source_root);
    if (configs.empty()) {
        printf("[%.*s] No config-generating rules found.\n", static_cast<int>(variant_name.size()), variant_name.data());
        ensure_config();
        return EXIT_SUCCESS;
    }

    // Filter config commands by scope if specified
    auto config_commands = std::set<pup::NodeId> {};
    for (auto const& cfg : configs) {
        auto const* node = ctx.graph().get_command_node(cfg.cmd_id);
        auto source_dir_sv = node ? pup::graph::get_source_dir(ctx.graph().graph(), cfg.cmd_id) : std::string_view {};
        if (!scopes.empty() && node && !pup::is_path_in_any_scope(std::string { source_dir_sv }, scopes)) {
            continue;
        }
        config_commands.insert(cfg.cmd_id);
        if (opts.verbose) {
            auto display_sv = node ? pup::graph::get_display_str(ctx.graph().graph(), cfg.cmd_id) : std::string_view { "<unknown>" };
            printf("[%.*s] Config rule: %.*s -> %s\n", static_cast<int>(variant_name.size()), variant_name.data(), static_cast<int>(display_sv.size()), display_sv.data(), cfg.output_path.c_str());
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
            printf("[%.*s] %s\n", static_cast<int>(variant_name.size()), variant_name.data(), job.display.c_str());
        }
    });

    scheduler.on_job_complete([&](pup::exec::BuildJob const& job, pup::exec::JobResult const& job_result) {
        if (!job_result.success) {
            fprintf(stderr, "[%.*s] FAILED: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), job.display.c_str());
            if (!job_result.output.empty()) {
                fprintf(stderr, "[%.*s] %s\n", static_cast<int>(variant_name.size()), variant_name.data(), job_result.output.c_str());
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
        fprintf(stderr, "[%.*s] Configure failed: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), build_result.error().message.c_str());
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
