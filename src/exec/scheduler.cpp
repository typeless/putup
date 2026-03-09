// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/exec/scheduler.hpp"
#include "pup/core/metrics.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/rule_pattern.hpp"
#include "pup/graph/topo.hpp"
#include "pup/parser/depfile.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace pup::exec {

namespace {

using EnvCache = std::unordered_map<std::string, std::string>;

/// Build immutable cache of environment variables for exported vars.
/// Must be called before spawning worker threads (getenv is not thread-safe).
auto build_env_cache(std::vector<BuildJob> const& jobs) -> EnvCache
{
    auto cache = EnvCache {};
    for (auto const& job : jobs) {
        for (auto const& var_name : job.exported_vars) {
            if (!cache.contains(var_name)) {
                if (auto const* env_val = std::getenv(var_name.c_str())) {
                    cache.emplace(var_name, env_val);
                }
            }
        }
    }
    return cache;
}

/// Resolve an output path to a filesystem path, handling variant-mapped paths.
/// If the path is already prefixed with the variant output directory, use source_root as base.
/// Otherwise, use output_root as base.
auto resolve_variant_path(
    std::filesystem::path const& source_root,
    std::filesystem::path const& output_root,
    std::string_view output_root_prefix,
    std::filesystem::path const& path
) -> std::filesystem::path
{
    auto path_str = path.generic_string();
    if (!output_root_prefix.empty() && path_str.starts_with(output_root_prefix)
        && (path_str.size() == output_root_prefix.size() || path_str[output_root_prefix.size()] == '/')) {
        return source_root / path;
    }
    return output_root / path;
}

/// Add job dependencies for any command that produces the given node.
/// For phi-nodes (multiple producers), only add active producers when consumer is active.
auto add_producer_dependencies(
    graph::BuildGraph const& graph,
    std::unordered_map<NodeId, std::size_t> const& cmd_to_job,
    std::vector<BuildJob> const& jobs,
    NodeId node_id,
    std::size_t current_job,
    std::unordered_set<std::size_t>& dependencies
) -> void
{
    auto current_active = jobs[current_job].guard_active;

    for (auto producer_id : graph.get_inputs(node_id)) {
        if (node_id::is_command(producer_id)) {
            if (auto it = cmd_to_job.find(producer_id); it != cmd_to_job.end() && it->second != current_job) {
                // Only add dependency if: current is inactive OR producer is active
                if (!current_active || jobs[it->second].guard_active) {
                    dependencies.insert(it->second);
                }
            }
        }
    }
}

/// Build dependency map between jobs using the graph's edge structure.
/// Returns: in_degree[j] = number of jobs that j depends on
///          dependents[i] = list of jobs that depend on job i
///
/// Uses NodeIds from the graph edges directly - no path string matching needed.
auto build_dependency_map(
    std::vector<BuildJob> const& jobs,
    graph::BuildGraph const& graph
) -> std::pair<std::vector<std::size_t>, std::vector<std::vector<std::size_t>>>
{
    auto in_degree = std::vector<std::size_t>(jobs.size(), 0);
    auto dependents = std::vector<std::vector<std::size_t>>(jobs.size());

    // Build map from command NodeId -> job index
    auto cmd_to_job = std::unordered_map<NodeId, std::size_t> {};
    for (auto i = std::size_t { 0 }; i < jobs.size(); ++i) {
        cmd_to_job[jobs[i].id] = i;
    }

    // For each job, find dependencies via input edges
    for (auto j = std::size_t { 0 }; j < jobs.size(); ++j) {
        auto dependencies = std::unordered_set<std::size_t> {};
        auto cmd_id = jobs[j].id;

        auto current_active = jobs[j].guard_active;

        // Check regular inputs - traverse graph edges
        for (auto input_id : graph.get_inputs(cmd_id)) {
            // Case 1: Input itself is a command (e.g., generated dep-scan rule)
            if (node_id::is_command(input_id)) {
                if (auto it = cmd_to_job.find(input_id); it != cmd_to_job.end() && it->second != j) {
                    // Apply guard filtering: skip inactive producers when current job is active
                    if (!current_active || jobs[it->second].guard_active) {
                        dependencies.insert(it->second);
                    }
                }
                continue;
            }

            // Case 2: Input is a file produced by another command
            add_producer_dependencies(graph, cmd_to_job, jobs, input_id, j, dependencies);
        }

        // Case 3: Order-only inputs (groups and files)
        // These establish ordering without creating true data dependencies.
        for (auto oo_id : graph.get_order_only(cmd_id)) {
            auto const* oo_node = graph.get_file_node(oo_id);
            if (!oo_node) {
                continue;
            }

            // For Group nodes, get member files and find their producers
            if (oo_node->type == NodeType::Group) {
                for (auto member_id : graph.get_inputs(oo_id)) {
                    add_producer_dependencies(graph, cmd_to_job, jobs, member_id, j, dependencies);
                }
            } else {
                add_producer_dependencies(graph, cmd_to_job, jobs, oo_id, j, dependencies);
            }
        }

        in_degree[j] = dependencies.size();
        for (auto dep : dependencies) {
            dependents[dep].push_back(j);
        }
    }

    return { std::move(in_degree), std::move(dependents) };
}

/// Validate that no active job depends on a skipped job's output.
/// Returns error if an active job would fail due to missing inputs from skipped jobs.
auto validate_guard_dependencies(
    std::vector<BuildJob> const& jobs,
    std::vector<std::vector<std::size_t>> const& dependents
) -> Result<void>
{
    for (auto i = std::size_t { 0 }; i < jobs.size(); ++i) {
        if (jobs[i].guard_active) {
            continue;
        }
        // This job is skipped - check if any active job depends on it
        for (auto dep_idx : dependents[i]) {
            if (jobs[dep_idx].guard_active) {
                return make_error<void>(
                    ErrorCode::MissingInput,
                    "Command '" + jobs[dep_idx].command
                        + "' depends on output from '" + jobs[i].command
                        + "' which is inactive due to conditional guard"
                );
            }
        }
    }
    return {};
}

/// Collect all commands required to build the given target nodes.
/// Uses reverse traversal: starts at targets, walks backward through inputs.
auto collect_required_commands(
    graph::BuildGraph const& graph,
    std::vector<NodeId> const& target_ids
) -> std::set<NodeId>
{
    auto visited = std::set<NodeId> {};
    auto commands = std::set<NodeId> {};
    auto stack = std::vector<NodeId>(target_ids.begin(), target_ids.end());

    while (!stack.empty()) {
        auto id = stack.back();
        stack.pop_back();

        if (visited.contains(id)) {
            continue;
        }
        visited.insert(id);

        if (node_id::is_command(id) && graph.get_command_node(id)) {
            commands.insert(id);
        }

        for (auto input_id : graph.get_inputs(id)) {
            stack.push_back(input_id);
        }

        for (auto dep_id : graph.get_order_only(id)) {
            stack.push_back(dep_id);
        }
    }

    return commands;
}

} // namespace

struct Scheduler::Impl {
    SchedulerOptions options;
    BuildStats stats;
    std::atomic<bool> cancelled = false;

    JobStartCallback on_start;
    JobCompleteCallback on_complete;
    ProgressCallback on_progress;
};

Scheduler::Scheduler(SchedulerOptions options)
    : impl_(std::make_unique<Impl>())
{
    impl_->options = std::move(options);
    if (impl_->options.jobs == 0) {
        impl_->options.jobs = detect_parallelism();
    }
}

Scheduler::~Scheduler() = default;

Scheduler::Scheduler(Scheduler&&) noexcept = default;

auto Scheduler::operator=(Scheduler&&) noexcept -> Scheduler& = default;

auto Scheduler::on_job_start(JobStartCallback callback) -> void
{
    impl_->on_start = std::move(callback);
}

auto Scheduler::on_job_complete(JobCompleteCallback callback) -> void
{
    impl_->on_complete = std::move(callback);
}

auto Scheduler::on_progress(ProgressCallback callback) -> void
{
    impl_->on_progress = std::move(callback);
}

auto Scheduler::cancel() -> void
{
    impl_->cancelled.store(true);
}

auto Scheduler::is_cancelled() const -> bool
{
    return impl_->cancelled.load();
}

auto Scheduler::stats() const -> BuildStats
{
    return impl_->stats;
}

auto Scheduler::build(graph::BuildGraph const& graph) -> Result<BuildStats>
{
    auto start_time = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
    impl_->cancelled.store(false);
    impl_->stats = BuildStats {};

    // Build job list in topological order
    auto jobs_result = Result<std::vector<BuildJob>> { build_job_list(graph) };
    if (!jobs_result) {
        return pup::unexpected<Error>(jobs_result.error());
    }

    auto& jobs = *jobs_result;
    impl_->stats.total_jobs = jobs.size();

    if (jobs.empty()) {
        auto end_time = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
        impl_->stats.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time
        );
        return impl_->stats;
    }

    // Execute jobs
    auto exec_result = Result<void> { execute_parallel(jobs, graph) };

    auto end_time = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
    impl_->stats.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time
    );

    if (!exec_result && !impl_->options.keep_going) {
        return pup::unexpected<Error>(exec_result.error());
    }

    return impl_->stats;
}

auto Scheduler::build_incremental(
    graph::BuildGraph const& graph,
    std::vector<std::string> const& changed_files
) -> Result<BuildStats>
{
    // Build temporary path-to-NodeId map for changed file lookup
    // This is O(n) scan but only done once per incremental build
    auto path_to_id = std::unordered_map<std::string, NodeId> {};
    for (auto id : graph.all_nodes()) {
        auto path = graph.get_full_path(id);
        if (!path.empty()) {
            path_to_id[path] = id;
        }
    }

    // Find all nodes affected by changes
    auto affected = std::set<NodeId> {};

    for (auto const& file_path : changed_files) {
        auto it = path_to_id.find(file_path);
        if (it != path_to_id.end()) {
            auto id = it->second;
            affected.insert(id);

            // For generated files that are missing/changed, also mark the producing command
            auto const* node = graph.get_file_node(id);
            if (node && node->type == NodeType::Generated) {
                for (auto input_id : graph.get_inputs(id)) {
                    affected.insert(input_id);
                }
            }
        }
    }

    // Expand to include all dependent commands (including order-only)
    // get_outputs() excludes sticky edges by design (Tupfile/config dependencies
    // are parse-time deps, not build-time deps)
    auto to_process = std::vector<NodeId>(affected.begin(), affected.end());

    while (!to_process.empty()) {
        auto id = NodeId { to_process.back() };
        to_process.pop_back();

        for (auto dep_id : graph.get_outputs(id)) {
            if (affected.insert(dep_id).second) {
                to_process.push_back(dep_id);
            }
        }

        for (auto dep_id : graph.get_order_only_dependents(id)) {
            if (affected.insert(dep_id).second) {
                to_process.push_back(dep_id);
            }
        }
    }

    // Build all jobs, then filter
    auto all_jobs = Result<std::vector<BuildJob>> { build_job_list(graph) };
    if (!all_jobs) {
        return pup::unexpected<Error>(all_jobs.error());
    }

    auto jobs = std::vector<BuildJob> { filter_jobs(*all_jobs, affected) };
    impl_->stats.total_jobs = jobs.size();
    impl_->stats.skipped_jobs = all_jobs->size() - jobs.size();

    if (jobs.empty()) {
        return impl_->stats;
    }

    auto exec_result = Result<void> { execute_parallel(jobs, graph) };
    if (!exec_result && !impl_->options.keep_going) {
        return pup::unexpected<Error>(exec_result.error());
    }

    return impl_->stats;
}

auto Scheduler::execute_sequential(
    std::vector<BuildJob> const& jobs,
    graph::BuildGraph const& graph,
    std::unordered_map<std::string, std::string> const& env_cache
) -> Result<void>
{
    auto runner = CommandRunner {};
    if (!impl_->options.source_root.empty()) {
        runner.set_working_dir(impl_->options.source_root);
    }
    if (impl_->options.timeout) {
        runner.set_timeout(*impl_->options.timeout); // NOLINT(bugprone-unchecked-optional-access)
    }

    auto [in_degree, dependents] = build_dependency_map(jobs, graph);

    // Validate no active job depends on a skipped job
    if (auto result = validate_guard_dependencies(jobs, dependents); !result) {
        return pup::unexpected<Error>(result.error());
    }

    // Count inactive jobs upfront (they never enter the queue)
    auto inactive_count = std::count_if(jobs.begin(), jobs.end(), [](auto const& j) { return !j.guard_active; });
    impl_->stats.skipped_jobs += static_cast<std::size_t>(inactive_count);

    // Only queue active jobs with no dependencies
    auto ready_queue = std::queue<std::size_t> {};
    for (auto i = std::size_t { 0 }; i < jobs.size(); ++i) {
        if (in_degree[i] == 0 && jobs[i].guard_active) {
            ready_queue.push(i);
        }
    }

    while (!ready_queue.empty()) {
        if (impl_->cancelled.load()) {
            break;
        }

        auto const job_idx = ready_queue.front();
        ready_queue.pop();
        auto const& job = jobs[job_idx];

        if (impl_->on_start) {
            impl_->on_start(job);
        }

        auto result = JobResult { execute_job(job, runner, env_cache) };

        if (impl_->on_complete) {
            impl_->on_complete(job, result);
        }

        impl_->stats.build_time += result.duration;

        if (result.success) {
            ++impl_->stats.completed_jobs;
            for (auto dep_idx : dependents[job_idx]) {
                if (--in_degree[dep_idx] == 0 && jobs[dep_idx].guard_active) {
                    ready_queue.push(dep_idx);
                }
            }
        } else {
            ++impl_->stats.failed_jobs;
            if (!impl_->options.keep_going) {
                return make_error<void>(ErrorCode::CommandFailed, "Command failed");
            }
        }

        if (impl_->on_progress) {
            impl_->on_progress(impl_->stats.completed_jobs + impl_->stats.failed_jobs, impl_->stats.total_jobs);
        }
    }

    return {};
}

auto Scheduler::execute_parallel(
    std::vector<BuildJob> const& jobs,
    graph::BuildGraph const& graph
) -> Result<void>
{
    auto const env_cache = build_env_cache(jobs);

    if (impl_->options.jobs == 1 || jobs.size() == 1) {
        return execute_sequential(jobs, graph, env_cache);
    }

    // Parallel execution with dependency-aware ready queue
    auto mutex = std::mutex {};
    auto cv = std::condition_variable {};
    auto ready_queue = std::queue<std::size_t> {};
    auto completed_count = std::size_t { 0 };
    auto failed = std::atomic<bool> { false };
    auto all_done = std::atomic<bool> { false };

    // Build dependency map using graph edges (no path string matching needed)
    auto [in_degree, dependents] = build_dependency_map(jobs, graph);

    // Validate no active job depends on a skipped job
    if (auto result = validate_guard_dependencies(jobs, dependents); !result) {
        return pup::unexpected<Error>(result.error());
    }

    // Count active jobs - inactive jobs never enter the queue
    auto active_count = static_cast<std::size_t>(std::count_if(jobs.begin(), jobs.end(), [](auto const& j) { return j.guard_active; }));
    impl_->stats.skipped_jobs += jobs.size() - active_count;

    // If no jobs are active, we're done
    if (active_count == 0) {
        return {};
    }

    // Only queue active jobs with no dependencies
    for (auto i = std::size_t { 0 }; i < jobs.size(); ++i) {
        if (in_degree[i] == 0 && jobs[i].guard_active) {
            ready_queue.push(i);
        }
    }

    auto worker = [&]() {
        auto runner = CommandRunner {};
        if (!impl_->options.source_root.empty()) {
            runner.set_working_dir(impl_->options.source_root);
        }
        if (impl_->options.timeout) {
            runner.set_timeout(*impl_->options.timeout);
        }

        while (true) {
            auto job_idx = std::size_t { 0 };
            {
                auto lock = std::unique_lock { mutex };
                cv.wait(lock, [&] {
                    // Wake up when: queue has ready jobs, OR all done, OR cancelled, OR failed
                    return !ready_queue.empty() || all_done.load() || impl_->cancelled.load() || (failed.load() && !impl_->options.keep_going);
                });

                if (impl_->cancelled.load() || (failed.load() && !impl_->options.keep_going)) {
                    return;
                }

                if (ready_queue.empty()) {
                    if (all_done.load()) {
                        return;
                    }
                    continue; // Spurious wakeup, wait again
                }

                job_idx = ready_queue.front();
                ready_queue.pop();
            }

            auto const& job = jobs[job_idx];

            if (impl_->on_start) {
                auto lock = std::lock_guard { mutex };
                impl_->on_start(job);
            }

            auto result = JobResult { execute_job(job, runner, env_cache) };

            {
                auto lock = std::lock_guard { mutex };

                if (impl_->on_complete) {
                    impl_->on_complete(job, result);
                }

                if (result.success) {
                    ++impl_->stats.completed_jobs;
                } else {
                    ++impl_->stats.failed_jobs;
                    failed.store(true);
                }

                ++completed_count;
                impl_->stats.build_time += result.duration;

                if (impl_->on_progress) {
                    impl_->on_progress(completed_count, impl_->stats.total_jobs);
                }

                // Queue active dependents whose dependencies are now satisfied
                for (auto dependent_idx : dependents[job_idx]) {
                    --in_degree[dependent_idx];
                    if (in_degree[dependent_idx] == 0 && jobs[dependent_idx].guard_active) {
                        ready_queue.push(dependent_idx);
                    }
                }

                // Check if all active jobs are done
                if (completed_count >= active_count) {
                    all_done.store(true);
                }
            }

            cv.notify_all();
        }
    };

    // Start worker threads
    auto threads = std::vector<std::thread> {};
    auto num_workers = std::min(impl_->options.jobs, jobs.size());
    threads.reserve(num_workers);

    for (auto i = std::size_t { 0 }; i < num_workers; ++i) {
        threads.emplace_back(worker);
    }

    // Notify workers to start
    cv.notify_all();

    // Wait for completion
    {
        auto lock = std::unique_lock { mutex };
        cv.wait(lock, [&] {
            return all_done.load() || impl_->cancelled.load() || (failed.load() && !impl_->options.keep_going);
        });
    }

    // Signal workers to exit
    all_done.store(true);
    cv.notify_all();

    // Join threads
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    if (failed.load() && !impl_->options.keep_going) {
        return make_error<void>(ErrorCode::CommandFailed, "Build failed");
    }

    return {};
}

auto Scheduler::execute_job(
    BuildJob const& job,
    CommandRunner& runner,
    std::unordered_map<std::string, std::string> const& env_cache
) -> JobResult
{
    auto result = JobResult {
        .id = job.id,
        .success = false,
        .exit_code = 0,
        .output = {},
        .duration = {},
    };

    if (impl_->options.dry_run) {
        result.success = true;
        return result;
    }

    // Ensure output directories exist
    // Note: create_directories() is idempotent and thread-safe
    // Output paths may be:
    // - Absolute: use as-is
    // - Already variant-mapped (starts with relative output_root prefix): use source_root as base
    // - Source-relative: prepend output_root
    auto source_root = impl_->options.source_root;
    auto relative_output_root = std::filesystem::relative(
        impl_->options.output_root,
        source_root
    );
    auto output_root_prefix = relative_output_root.generic_string();
    for (auto const& output : job.outputs) {
        auto output_path = std::filesystem::path { output };
        if (!output_path.is_absolute()) {
            output_path = resolve_variant_path(source_root, impl_->options.output_root, output_root_prefix, output);
        }
        auto parent = output_path.parent_path();
        if (!parent.empty()) {
            auto ec = std::error_code {};
            std::filesystem::create_directories(parent, ec);
        }
    }

    auto run_opts = RunOptions {};
    if (!job.working_dir.empty()) {
        run_opts.working_dir = job.working_dir;
    }

    // Pass exported environment variables to command
    // Per tup manual: "value for the variable comes from tup's environment"
    for (auto const& var_name : job.exported_vars) {
        if (auto it = env_cache.find(var_name); it != env_cache.end()) {
            run_opts.env.push_back(var_name + "=" + it->second);
        }
    }

    auto cmd_result = runner.run(job.command, run_opts);
    if (!cmd_result) {
        result.output = "Failed to execute command";
        return result;
    }

    result.exit_code = cmd_result->exit_code;
    result.success = (cmd_result->exit_code == 0);
    result.duration = cmd_result->duration;

    if (!cmd_result->stdout_output.empty()) {
        result.output = cmd_result->stdout_output;
    }
    if (!cmd_result->stderr_output.empty()) {
        if (!result.output.empty()) {
            result.output += '\n';
        }
        result.output += cmd_result->stderr_output;
    }

    // Discover implicit dependencies from .d files (if command succeeded)
    if (result.success) {
        // For generated rules with inject_implicit_deps, parse stdout as depfile
        if (job.inject_implicit_deps && !cmd_result->stdout_output.empty()) {
            auto depfile_result = parser::parse_depfile(std::string_view { cmd_result->stdout_output });
            if (depfile_result) {
                for (auto& dep : depfile_result->dependencies) {
                    result.discovered_deps.push_back(std::move(dep));
                }
                result.deps_for_command = job.parent_command;
            }
        }

        // Traditional .d file discovery
        for (auto const& output : job.outputs) {
            auto output_path = std::filesystem::path { output };
            auto ext = output_path.extension().string();

            // Support common object file extensions (.o on Unix, .obj on Windows)
            if (ext != ".o" && ext != ".obj") {
                continue;
            }

            // Compute filesystem path for the .d file
            auto base_path = resolve_variant_path(source_root, impl_->options.output_root, output_root_prefix, output_path.parent_path());
            auto depfile_path = base_path / (output_path.stem().string() + ".d");

            if (!std::filesystem::exists(depfile_path)) {
                continue;
            }

            auto depfile_result = parser::parse_depfile(depfile_path);
            if (depfile_result) {
                for (auto& dep : depfile_result->dependencies) {
                    result.discovered_deps.push_back(std::move(dep));
                }
            }
        }
    }

    return result;
}

auto Scheduler::build_job_list(
    graph::BuildGraph const& graph
) -> Result<std::vector<BuildJob>>
{
    auto job_list_start = std::chrono::high_resolution_clock::now();

    // Get topological order
    auto topo_result = graph::TopoSortResult { graph::topological_sort(graph) };
    if (topo_result.has_cycle) {
        return make_error<std::vector<BuildJob>>(
            ErrorCode::CyclicDependency, "Dependency cycle detected"
        );
    }

    // Validate no unrealized ghost nodes remain (missing inputs)
    // Exception: Ghost nodes whose files actually exist on disk are valid
    // non-generated input files (e.g., tup.config, manually-created config files)
    for (auto id : topo_result.order) {
        auto const* node = graph.get_file_node(id);
        if (node && node->type == NodeType::Ghost && !graph.get_outputs(id).empty()) {
            auto path = graph.get_full_path(id);
            // Check if file exists - if so, it's a valid input (not missing)
            auto build_root_name = std::string { graph.get_build_root_name() };
            auto file_path = impl_->options.output_root / path;
            // Strip build prefix from path if present (for consistent file lookup)
            auto lookup_path = path;
            auto build_prefix = build_root_name + "/";
            if (!build_root_name.empty() && path.starts_with(build_prefix)) {
                lookup_path = path.substr(build_prefix.size());
                file_path = impl_->options.output_root / lookup_path;
            }
            if (std::filesystem::exists(file_path)) {
                continue; // File exists, not a missing input
            }
            return make_error<std::vector<BuildJob>>(
                ErrorCode::ParseError,
                "Missing input file (unresolved ghost): " + path
            );
        }
    }

    auto jobs = std::vector<BuildJob> {};
    auto cache = graph::PathCache {};

    for (auto id : topo_result.order) {
        if (!node_id::is_command(id)) {
            continue;
        }
        auto const* node = graph.get_command_node(id);
        if (!node) {
            continue;
        }

        // Compute working directory: source_dir for subdirectory Tupfiles.
        // Commands run from the Tupfile's SOURCE directory so that relative paths
        // and TUP_VARIANT_OUTPUTDIR work correctly. Output paths are already
        // mapped to the output directory by the builder.
        auto source_dir = get_source_dir(graph.graph(), id);
        auto working_dir = std::filesystem::path { impl_->options.source_root };
        if (!source_dir.empty()) {
            working_dir /= source_dir;
        }

        // Check if this is a generated rule that captures stdout
        auto capture_stdout = false;
        auto inject_implicit = false;
        auto parent_cmd = INVALID_NODE_ID;
        if (node->generated_output && node->generated_output->type == graph::GeneratedOutput::Type::Stdout) {
            capture_stdout = true;
            if (node->output_action == graph::OutputAction::InjectImplicitDeps) {
                inject_implicit = true;
                parent_cmd = node->parent_command;
            }
        }

        // Expand command from instruction pattern + operands
        auto cmd_str = expand_instruction(graph.graph(), id, cache, impl_->options.source_root, impl_->options.config_root);
        auto display_str = std::string { get_display_str(graph.graph(), id) };

        // Convert exported_vars from StringIds to strings
        auto exported_str = std::set<std::string> {};
        for (auto var_id : node->exported_vars) {
            exported_str.insert(std::string { graph.graph().strings.get(var_id) });
        }

        // Evaluate guards - command only executes if ALL guards are satisfied
        auto guard_active = graph::is_guard_satisfied(graph.graph(), *node);

        auto job = BuildJob {
            .id = id,
            .command = cmd_str,
            .display = display_str.empty() ? cmd_str : display_str,
            .working_dir = working_dir,
            .inputs = {},
            .outputs = {},
            .order_only_inputs = {},
            .exported_vars = std::move(exported_str),
            .capture_stdout = capture_stdout,
            .inject_implicit_deps = inject_implicit,
            .parent_command = parent_cmd,
            .guard_active = guard_active,
        };

        // Collect input paths
        for (auto input_id : graph.get_inputs(id)) {
            auto input_path = graph.get_full_path(input_id);
            if (!input_path.empty()) {
                job.inputs.push_back(std::move(input_path));
            }
        }

        // Collect output paths
        for (auto output_id : graph.get_outputs(id)) {
            auto output_path = graph.get_full_path(output_id);
            if (!output_path.empty()) {
                job.outputs.push_back(std::move(output_path));
            }
        }

        // Collect order-only input paths
        // For Group nodes, expand to member file paths
        for (auto oi_id : graph.get_order_only(id)) {
            auto const* oi_node = graph.get_file_node(oi_id);
            if (!oi_node) {
                continue;
            }

            if (oi_node->type == NodeType::Group) {
                // Group nodes: collect member file paths
                for (auto member_id : graph.get_inputs(oi_id)) {
                    auto member_path = graph.get_full_path(member_id);
                    if (!member_path.empty()) {
                        job.order_only_inputs.push_back(std::move(member_path));
                    }
                }
            } else {
                auto oi_path = graph.get_full_path(oi_id);
                if (!oi_path.empty()) {
                    job.order_only_inputs.push_back(std::move(oi_path));
                }
            }
        }

        jobs.push_back(std::move(job));
    }

    auto job_list_elapsed = std::chrono::high_resolution_clock::now() - job_list_start;
    thread_metrics().job_list_time += std::chrono::duration_cast<std::chrono::microseconds>(job_list_elapsed);

    return jobs;
}

auto Scheduler::build_subset(
    graph::BuildGraph const& graph,
    std::set<NodeId> const& command_ids
) -> Result<BuildStats>
{
    auto start_time = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
    impl_->cancelled.store(false);
    impl_->stats = BuildStats {};

    // Build all jobs, then filter to the subset
    auto all_jobs = Result<std::vector<BuildJob>> { build_job_list(graph) };
    if (!all_jobs) {
        return pup::unexpected<Error>(all_jobs.error());
    }

    auto jobs = filter_jobs(*all_jobs, command_ids);
    impl_->stats.total_jobs = jobs.size();
    impl_->stats.skipped_jobs = all_jobs->size() - jobs.size();

    if (jobs.empty()) {
        auto end_time = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
        impl_->stats.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time
        );
        return impl_->stats;
    }

    auto exec_result = Result<void> { execute_parallel(jobs, graph) };

    auto end_time = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
    impl_->stats.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time
    );

    if (!exec_result && !impl_->options.keep_going) {
        return pup::unexpected<Error>(exec_result.error());
    }

    return impl_->stats;
}

auto Scheduler::build_targets(
    graph::BuildGraph const& graph,
    std::vector<NodeId> const& target_ids
) -> Result<BuildStats>
{
    auto start_time = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
    impl_->cancelled.store(false);
    impl_->stats = BuildStats {};

    // Collect all commands needed to build these targets via reverse traversal
    auto required_cmds = std::set<NodeId> { collect_required_commands(graph, target_ids) };

    // Build all jobs, then filter to required commands
    auto all_jobs = Result<std::vector<BuildJob>> { build_job_list(graph) };
    if (!all_jobs) {
        return pup::unexpected<Error>(all_jobs.error());
    }

    auto jobs = filter_jobs(*all_jobs, required_cmds);
    impl_->stats.total_jobs = jobs.size();
    impl_->stats.skipped_jobs = all_jobs->size() - jobs.size();

    if (jobs.empty()) {
        auto end_time = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
        impl_->stats.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time
        );
        return impl_->stats;
    }

    auto exec_result = Result<void> { execute_parallel(jobs, graph) };

    auto end_time = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
    impl_->stats.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time
    );

    if (!exec_result && !impl_->options.keep_going) {
        return pup::unexpected<Error>(exec_result.error());
    }

    return impl_->stats;
}

auto Scheduler::filter_jobs(
    std::vector<BuildJob> const& all_jobs,
    std::set<NodeId> const& affected_nodes
) -> std::vector<BuildJob>
{
    auto result = std::vector<BuildJob> {};
    result.reserve(all_jobs.size());

    for (auto const& job : all_jobs) {
        if (affected_nodes.contains(job.id)) {
            result.push_back(job);
        }
    }

    return result;
}

auto detect_parallelism() -> std::size_t
{
    auto hw = std::size_t { std::thread::hardware_concurrency() };
    return hw > 0 ? hw : 1;
}

} // namespace pup::exec
