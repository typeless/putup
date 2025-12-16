// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/exec/scheduler.hpp"
#include "pup/core/hash.hpp"
#include "pup/graph/rule_pattern.hpp"
#include "pup/graph/topo.hpp"
#include "pup/parser/depfile.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <queue>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

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

/// Build dependency map between jobs using the graph's edge structure.
/// Returns: in_degree[j] = number of jobs that j depends on
///          dependents[i] = list of jobs that depend on job i
///
/// Uses NodeIds from the graph edges directly - no path string matching needed.
auto build_dependency_map(
    std::vector<BuildJob> const& jobs,
    graph::BuildGraph const& graph) -> std::pair<std::vector<std::size_t>, std::vector<std::vector<std::size_t>>>
{
    auto in_degree = std::vector<std::size_t>(jobs.size(), 0);
    auto dependents = std::vector<std::vector<std::size_t>>(jobs.size());

    // Build map from command NodeId -> job index
    auto cmd_to_job = std::unordered_map<NodeId, std::size_t> {};
    for (auto i = std::size_t { 0 }; i < jobs.size(); ++i) {
        cmd_to_job[jobs[i].id] = i;
    }

    // Build map from output NodeId -> job index that produces it
    // (output nodes point back to the command that created them via inputs)
    auto output_to_job = std::unordered_map<NodeId, std::size_t> {};
    for (auto i = std::size_t { 0 }; i < jobs.size(); ++i) {
        for (auto output_id : graph.get_outputs(jobs[i].id)) {
            output_to_job[output_id] = i;
        }
    }

    // For each job, find dependencies via input edges
    for (auto j = std::size_t { 0 }; j < jobs.size(); ++j) {
        auto dependencies = std::set<std::size_t> {};
        auto cmd_id = jobs[j].id;

        // Check regular inputs - traverse graph edges
        for (auto input_id : graph.get_inputs(cmd_id)) {
            auto const* input_node = graph.get_node(input_id);
            if (!input_node) {
                continue;
            }

            // Case 1: Input itself is a command (e.g., generated dep-scan rule)
            if (input_node->type == NodeType::Command) {
                if (auto it = cmd_to_job.find(input_id); it != cmd_to_job.end() && it->second != j) {
                    dependencies.insert(it->second);
                }
                continue;
            }

            // Case 2: Input is a file produced by another command
            for (auto producer_id : graph.get_inputs(input_id)) {
                auto const* producer = graph.get_node(producer_id);
                if (producer && producer->type == NodeType::Command) {
                    if (auto it = cmd_to_job.find(producer_id); it != cmd_to_job.end() && it->second != j) {
                        dependencies.insert(it->second);
                    }
                }
            }
        }

        // Check order-only inputs - these may be outputs of other commands or groups
        for (auto oo_id : graph.get_order_only(cmd_id)) {
            // Check if order-only input is a direct output of another job
            if (auto it = output_to_job.find(oo_id); it != output_to_job.end() && it->second != j) {
                dependencies.insert(it->second);
            }

            // Also check for group producers (groups have commands as inputs)
            for (auto producer_id : graph.get_inputs(oo_id)) {
                auto const* producer = graph.get_node(producer_id);
                if (producer && producer->type == NodeType::Command) {
                    if (auto it2 = cmd_to_job.find(producer_id); it2 != cmd_to_job.end() && it2->second != j) {
                        dependencies.insert(it2->second);
                    }
                }
            }
        }

        in_degree[j] = dependencies.size();
        for (auto dep : dependencies) {
            dependents[dep].push_back(j);
        }
    }

    return { std::move(in_degree), std::move(dependents) };
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
            end_time - start_time);
        return impl_->stats;
    }

    // Execute jobs
    auto exec_result = Result<void> { execute_parallel(jobs, graph) };

    auto end_time = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
    impl_->stats.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);

    if (!exec_result && !impl_->options.keep_going) {
        return pup::unexpected<Error>(exec_result.error());
    }

    return impl_->stats;
}

auto Scheduler::build_incremental(
    graph::BuildGraph const& graph,
    index::Index const& /*old_index*/,
    std::vector<std::string> const& changed_files) -> Result<BuildStats>
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
            auto const* node = graph.get_node(id);
            if (node && node->type == NodeType::Generated) {
                for (auto input_id : graph.get_inputs(id)) {
                    affected.insert(input_id);
                }
            }
        }
    }

    // Expand to include all dependent commands (including order-only)
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

auto Scheduler::execute_parallel(std::vector<BuildJob> const& jobs, graph::BuildGraph const& graph) -> Result<void>
{
    // Build immutable env cache before spawning workers (getenv is not thread-safe)
    auto const env_cache = build_env_cache(jobs);

    if (impl_->options.jobs == 1 || jobs.size() == 1) {
        // Sequential execution
        auto runner = CommandRunner {};
        if (!impl_->options.source_root.empty()) {
            runner.set_working_dir(impl_->options.source_root);
        }
        if (impl_->options.timeout) {
            // FIXME: False positive - optional is checked on previous line
            runner.set_timeout(*impl_->options.timeout); // NOLINT(bugprone-unchecked-optional-access)
        }

        for (auto const& job : jobs) {
            if (impl_->cancelled.load()) {
                break;
            }

            if (impl_->on_start) {
                impl_->on_start(job);
            }

            auto result = JobResult { execute_job(job, runner, env_cache) };

            if (impl_->on_complete) {
                impl_->on_complete(job, result);
            }

            if (result.success) {
                ++impl_->stats.completed_jobs;
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

    // Parallel execution with dependency-aware ready queue
    auto mutex = std::mutex {};
    auto cv = std::condition_variable {};
    auto ready_queue = std::queue<std::size_t> {};
    auto completed_count = std::size_t { 0 };
    auto failed = std::atomic<bool> { false };
    auto all_done = std::atomic<bool> { false };

    // Build dependency map using graph edges (no path string matching needed)
    auto [in_degree, dependents] = build_dependency_map(jobs, graph);

    // Initialize ready queue with jobs that have no dependencies
    for (auto i = std::size_t { 0 }; i < jobs.size(); ++i) {
        if (in_degree[i] == 0) {
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

                // Queue dependents whose dependencies are now satisfied
                for (auto dependent_idx : dependents[job_idx]) {
                    --in_degree[dependent_idx];
                    if (in_degree[dependent_idx] == 0) {
                        ready_queue.push(dependent_idx);
                    }
                }

                // Check if all jobs are done
                if (completed_count >= jobs.size()) {
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

auto Scheduler::execute_job(BuildJob const& job, CommandRunner& runner,
    std::unordered_map<std::string, std::string> const& env_cache) -> JobResult
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
    // Output paths are relative to source_root (e.g., "build-s1f3/tools/modHeader")
    for (auto const& output : job.outputs) {
        auto output_path = std::filesystem::path { output };
        if (!output_path.is_absolute()) {
            output_path = impl_->options.source_root / output;
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

            auto depfile_path = std::filesystem::path {
                impl_->options.source_root / output_path.parent_path() / (output_path.stem().string() + ".d")
            };

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
    graph::BuildGraph const& graph) -> Result<std::vector<BuildJob>>
{
    // Get topological order
    auto topo_result = graph::TopoSortResult { graph::topological_sort(graph) };
    if (topo_result.has_cycle) {
        return make_error<std::vector<BuildJob>>(
            ErrorCode::CyclicDependency, "Dependency cycle detected");
    }

    auto jobs = std::vector<BuildJob> {};

    for (auto id : topo_result.order) {
        auto const* node = graph.get_node(id);
        if (!node || node->type != NodeType::Command) {
            continue;
        }

        // Compute working directory: source_dir for subdirectory Tupfiles.
        // Commands run from the Tupfile's SOURCE directory so that relative paths
        // and TUP_VARIANT_OUTPUTDIR work correctly. Output paths are already
        // mapped to the output directory by the builder.
        auto working_dir = std::filesystem::path { impl_->options.source_root };
        if (!node->source_dir.empty()) {
            working_dir /= node->source_dir;
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

        auto job = BuildJob {
            .id = id,
            .command = node->command,
            .display = node->display.empty() ? node->command : node->display,
            .working_dir = working_dir,
            .inputs = {},
            .outputs = {},
            .order_only_inputs = {},
            .exported_vars = node->exported_vars,
            .capture_stdout = capture_stdout,
            .inject_implicit_deps = inject_implicit,
            .parent_command = parent_cmd,
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
        for (auto oi_id : graph.get_order_only(id)) {
            auto oi_path = graph.get_full_path(oi_id);
            if (!oi_path.empty()) {
                job.order_only_inputs.push_back(std::move(oi_path));
            }
        }

        jobs.push_back(std::move(job));
    }

    return jobs;
}

auto Scheduler::filter_jobs(
    std::vector<BuildJob> const& all_jobs,
    std::set<NodeId> const& affected_nodes) -> std::vector<BuildJob>
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
