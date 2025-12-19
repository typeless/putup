// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/multi_variant.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/types.hpp"
#include "pup/exec/scheduler.hpp"
#include "pup/graph/dag.hpp"

#include <cstdlib>
#include <set>

#include <fmt/core.h>

namespace pup::cli {

namespace {

auto is_config_output(std::string const& path) -> bool
{
    return path.ends_with("tup.config");
}

/// Collect all commands that the given commands depend on (transitively).
/// This ensures that if a config rule depends on an intermediate file,
/// the command that produces that file is also included.
auto collect_dependencies(
    pup::graph::BuildGraph const& graph,
    std::set<pup::NodeId> const& commands
) -> std::set<pup::NodeId>
{
    auto result = std::set<pup::NodeId> { commands };
    auto worklist = std::vector<pup::NodeId> { commands.begin(), commands.end() };

    while (!worklist.empty()) {
        auto cmd_id = worklist.back();
        worklist.pop_back();

        // Check all inputs of this command
        for (auto input_id : graph.get_inputs(cmd_id)) {
            auto const* input_node = graph.get_node(input_id);
            if (!input_node) {
                continue;
            }

            // If input is a command, add it
            if (input_node->type == pup::NodeType::Command) {
                if (result.insert(input_id).second) {
                    worklist.push_back(input_id);
                }
                continue;
            }

            // If input is a file, find the command that produces it
            for (auto producer_id : graph.get_inputs(input_id)) {
                auto const* producer = graph.get_node(producer_id);
                if (producer && producer->type == pup::NodeType::Command) {
                    if (result.insert(producer_id).second) {
                        worklist.push_back(producer_id);
                    }
                }
            }
        }

        // Also check order-only dependencies
        for (auto oo_id : graph.get_order_only(cmd_id)) {
            for (auto producer_id : graph.get_inputs(oo_id)) {
                auto const* producer = graph.get_node(producer_id);
                if (producer && producer->type == pup::NodeType::Command) {
                    if (result.insert(producer_id).second) {
                        worklist.push_back(producer_id);
                    }
                }
            }
        }
    }

    return result;
}

auto configure_single_variant(
    Options const& opts,
    std::string_view /*variant_name*/
) -> int
{
    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
        .keep_going = opts.keep_going,
        .auto_init = false,       // Configure pass should not create .pup
        .root_config_only = true, // Use only root tup.config
    };

    auto result = build_context(opts, ctx_opts);
    if (!result) {
        fmt::print(stderr, "Error: {}\n", result.error().message);
        return EXIT_FAILURE;
    }

    auto& ctx = *result;

    // Find commands that output tup.config files
    auto config_commands = std::set<pup::NodeId> {};
    for (auto id : ctx.graph().all_nodes()) {
        auto const* node = ctx.graph().get_node(id);
        if (!node || node->type != pup::NodeType::Command) {
            continue;
        }

        // Check if any output ends with tup.config
        for (auto output_id : ctx.graph().get_outputs(id)) {
            auto output_path = ctx.graph().get_full_path(output_id);
            if (is_config_output(output_path)) {
                config_commands.insert(id);
                if (opts.verbose) {
                    fmt::print("Config rule: {} -> {}\n", node->display, output_path);
                }
                break;
            }
        }
    }

    if (config_commands.empty()) {
        fmt::print("No config-generating rules found.\n");
        return EXIT_SUCCESS;
    }

    // Expand to include all transitive dependencies
    // (commands that produce files needed by config rules)
    auto all_commands = collect_dependencies(ctx.graph(), config_commands);
    auto dep_count = all_commands.size() - config_commands.size();

    if (dep_count > 0 && opts.verbose) {
        fmt::print("Config rules depend on {} additional command(s)\n", dep_count);
    }

    fmt::print("Found {} config-generating rule(s)\n", config_commands.size());

    // Build only config-generating commands
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
            fmt::print("{}\n", job.display);
        }
    });

    scheduler.on_job_complete([&](pup::exec::BuildJob const& job, pup::exec::JobResult const& job_result) {
        if (!job_result.success) {
            fmt::print(stderr, "FAILED: {}\n", job.display);
            if (!job_result.output.empty()) {
                fmt::print(stderr, "{}\n", job_result.output);
            }
        }
    });

    scheduler.on_progress([&](std::size_t done, std::size_t total) {
        if (!opts.verbose) {
            fmt::print("\r[{}/{}] ", done, total);
            std::fflush(stdout);
        }
    });

    // Execute config-generating commands and their dependencies
    auto build_result = scheduler.build_subset(ctx.graph(), all_commands);

    if (!opts.verbose) {
        fmt::print("\n");
    }

    if (!build_result) {
        fmt::print(stderr, "Configure failed: {}\n", build_result.error().message);
        return EXIT_FAILURE;
    }

    auto const& stats = *build_result;
    if (stats.failed_jobs > 0) {
        fmt::print("Configure completed: {} commands ({} failed)\n", stats.completed_jobs, stats.failed_jobs);
        return EXIT_FAILURE;
    }

    fmt::print("Configure completed: {} commands\n", stats.completed_jobs);

    // NOTE: We do NOT write index - configure pass should not interfere with subsequent build

    return EXIT_SUCCESS;
}

} // anonymous namespace

auto cmd_configure(Options const& opts) -> int
{
    return for_each_variant(opts, configure_single_variant, "Configuring");
}

} // namespace pup::cli
