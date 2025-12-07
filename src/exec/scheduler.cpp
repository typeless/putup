// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/exec/scheduler.hpp"
#include "pup/core/hash.hpp"
#include "pup/graph/topo.hpp"

#include <algorithm>
#include <condition_variable>
#include <queue>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

namespace pup::exec {

namespace {

/// Build dependency map between jobs based on input/output relationships.
/// Returns: in_degree[j] = number of jobs that j depends on
///          dependents[i] = list of jobs that depend on job i
auto build_dependency_map(std::vector<BuildJob> const& jobs)
    -> std::pair<std::vector<std::size_t>, std::vector<std::vector<std::size_t>>>
{
    auto in_degree = std::vector<std::size_t>(jobs.size(), 0);
    auto dependents = std::vector<std::vector<std::size_t>>(jobs.size());

    // Build map from output path -> job index that produces it
    auto output_to_job = std::unordered_map<std::string, std::size_t>{};
    for (auto i = std::size_t{0}; i < jobs.size(); ++i) {
        for (auto const& output : jobs[i].outputs)
            output_to_job[output] = i;
    }

    // For each job, find which earlier jobs produce its inputs
    for (auto j = std::size_t{0}; j < jobs.size(); ++j) {
        auto dependencies = std::set<std::size_t>{};

        // Check regular inputs
        for (auto const& input : jobs[j].inputs) {
            auto it = output_to_job.find(input);
            if (it != output_to_job.end() && it->second != j)
                dependencies.insert(it->second);
        }

        // Check order-only inputs
        for (auto const& input : jobs[j].order_only_inputs) {
            auto it = output_to_job.find(input);
            if (it != output_to_job.end() && it->second != j)
                dependencies.insert(it->second);
        }

        in_degree[j] = dependencies.size();
        for (auto dep : dependencies)
            dependents[dep].push_back(j);
    }

    return {std::move(in_degree), std::move(dependents)};
}

} // namespace

Scheduler::Scheduler(SchedulerOptions options)
    : options_(std::move(options))
{
    if (options_.jobs == 0)
        options_.jobs = detect_parallelism();
}

auto Scheduler::build(graph::BuildGraph const& graph) -> Result<BuildStats>
{
    auto start_time = std::chrono::steady_clock::time_point{std::chrono::steady_clock::now()};
    cancelled_.store(false);
    stats_ = BuildStats {};

    // Build job list in topological order
    auto jobs_result = Result<std::vector<BuildJob>>{build_job_list(graph)};
    if (!jobs_result)
        return pup::unexpected<Error>(jobs_result.error());

    auto& jobs = *jobs_result;
    stats_.total_jobs = jobs.size();

    if (jobs.empty()) {
        auto end_time = std::chrono::steady_clock::time_point{std::chrono::steady_clock::now()};
        stats_.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);
        return stats_;
    }

    // Execute jobs
    auto exec_result = Result<void>{execute_parallel(jobs)};

    auto end_time = std::chrono::steady_clock::time_point{std::chrono::steady_clock::now()};
    stats_.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);

    if (!exec_result && !options_.keep_going)
        return pup::unexpected<Error>(exec_result.error());

    return stats_;
}

auto Scheduler::build_incremental(
    graph::BuildGraph const& graph,
    index::Index const& /*old_index*/,
    std::vector<std::string> const& changed_files) -> Result<BuildStats>
{
    // Find all nodes affected by changes
    auto affected = std::set<NodeId> {};

    for (auto const& path : changed_files) {
        if (auto id = graph.find_by_path(path))
            affected.insert(*id);
    }

    // Expand to include all dependent commands
    auto to_process = std::vector<NodeId>(affected.begin(), affected.end());
    while (!to_process.empty()) {
        auto id = NodeId{to_process.back()};
        to_process.pop_back();

        for (auto dep_id : graph.get_outputs(id)) {
            if (affected.insert(dep_id).second)
                to_process.push_back(dep_id);
        }
    }

    // Build all jobs, then filter
    auto all_jobs = Result<std::vector<BuildJob>>{build_job_list(graph)};
    if (!all_jobs)
        return pup::unexpected<Error>(all_jobs.error());

    auto jobs = std::vector<BuildJob>{filter_jobs(*all_jobs, affected)};
    stats_.total_jobs = jobs.size();
    stats_.skipped_jobs = all_jobs->size() - jobs.size();

    if (jobs.empty())
        return stats_;

    auto exec_result = Result<void>{execute_parallel(jobs)};
    if (!exec_result && !options_.keep_going)
        return pup::unexpected<Error>(exec_result.error());

    return stats_;
}

auto Scheduler::execute_parallel(std::vector<BuildJob> const& jobs) -> Result<void>
{
    if (options_.jobs == 1 || jobs.size() == 1) {
        // Sequential execution
        auto runner = CommandRunner {};
        if (!options_.root_dir.empty())
            runner.set_working_dir(options_.root_dir);
        if (options_.timeout)
            runner.set_timeout(*options_.timeout);

        for (auto const& job : jobs) {
            if (cancelled_.load())
                break;

            if (on_start_)
                on_start_(job);

            auto result = JobResult{execute_job(job, runner)};

            if (on_complete_)
                on_complete_(job, result);

            if (result.success) {
                ++stats_.completed_jobs;
            } else {
                ++stats_.failed_jobs;
                if (!options_.keep_going)
                    return make_error<void>(ErrorCode::CommandFailed, "Command failed");
            }

            if (on_progress_)
                on_progress_(stats_.completed_jobs + stats_.failed_jobs, stats_.total_jobs);
        }

        return {};
    }

    // Parallel execution with dependency-aware ready queue
    auto mutex = std::mutex{};
    auto cv = std::condition_variable{};
    auto ready_queue = std::queue<std::size_t>{};
    auto completed_count = std::size_t{0};
    auto failed = std::atomic<bool>{false};
    auto all_done = std::atomic<bool>{false};

    // Build dependency map
    auto [in_degree, dependents] = build_dependency_map(jobs);

    // Initialize ready queue with jobs that have no dependencies
    for (auto i = std::size_t{0}; i < jobs.size(); ++i) {
        if (in_degree[i] == 0)
            ready_queue.push(i);
    }

    auto worker = [&]() {
        auto runner = CommandRunner{};
        if (!options_.root_dir.empty())
            runner.set_working_dir(options_.root_dir);
        if (options_.timeout)
            runner.set_timeout(*options_.timeout);

        while (true) {
            auto job_idx = std::size_t{0};
            {
                auto lock = std::unique_lock{mutex};
                cv.wait(lock, [&] {
                    // Wake up when: queue has ready jobs, OR all done, OR cancelled, OR failed
                    return !ready_queue.empty() || all_done.load() ||
                           cancelled_.load() || (failed.load() && !options_.keep_going);
                });

                if (cancelled_.load() || (failed.load() && !options_.keep_going))
                    return;

                if (ready_queue.empty()) {
                    if (all_done.load())
                        return;
                    continue; // Spurious wakeup, wait again
                }

                job_idx = ready_queue.front();
                ready_queue.pop();
            }

            auto const& job = jobs[job_idx];

            if (on_start_) {
                auto lock = std::lock_guard{mutex};
                on_start_(job);
            }

            auto result = JobResult{execute_job(job, runner)};

            {
                auto lock = std::lock_guard{mutex};

                if (on_complete_)
                    on_complete_(job, result);

                if (result.success) {
                    ++stats_.completed_jobs;
                } else {
                    ++stats_.failed_jobs;
                    failed.store(true);
                }

                ++completed_count;
                stats_.build_time += result.duration;

                if (on_progress_)
                    on_progress_(completed_count, stats_.total_jobs);

                // Queue dependents whose dependencies are now satisfied
                for (auto dependent_idx : dependents[job_idx]) {
                    --in_degree[dependent_idx];
                    if (in_degree[dependent_idx] == 0)
                        ready_queue.push(dependent_idx);
                }

                // Check if all jobs are done
                if (completed_count >= jobs.size())
                    all_done.store(true);
            }

            cv.notify_all();
        }
    };

    // Start worker threads
    auto threads = std::vector<std::thread>{};
    auto num_workers = std::min(options_.jobs, jobs.size());
    threads.reserve(num_workers);

    for (auto i = std::size_t{0}; i < num_workers; ++i)
        threads.emplace_back(worker);

    // Notify workers to start
    cv.notify_all();

    // Wait for completion
    {
        auto lock = std::unique_lock{mutex};
        cv.wait(lock, [&] {
            return all_done.load() || cancelled_.load() || (failed.load() && !options_.keep_going);
        });
    }

    // Signal workers to exit
    all_done.store(true);
    cv.notify_all();

    // Join threads
    for (auto& t : threads) {
        if (t.joinable())
            t.join();
    }

    if (failed.load() && !options_.keep_going)
        return make_error<void>(ErrorCode::CommandFailed, "Build failed");

    return {};
}

auto Scheduler::execute_job(BuildJob const& job, CommandRunner& runner) -> JobResult
{
    auto result = JobResult {
        .id = job.id,
        .success = false,
        .exit_code = 0,
        .output = {},
        .duration = {},
    };

    if (options_.dry_run) {
        result.success = true;
        return result;
    }

    // Ensure output directories exist
    for (auto const& output : job.outputs) {
        auto output_path = std::filesystem::path{options_.root_dir / output};
        auto parent = std::filesystem::path{output_path.parent_path()};
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
    }

    auto run_opts = RunOptions {};
    if (!job.working_dir.empty())
        run_opts.working_dir = job.working_dir;

    auto cmd_result = runner.run(job.command, run_opts);
    if (!cmd_result) {
        result.output = "Failed to execute command";
        return result;
    }

    result.exit_code = cmd_result->exit_code;
    result.success = (cmd_result->exit_code == 0);
    result.duration = cmd_result->duration;

    if (!cmd_result->stdout_output.empty())
        result.output = cmd_result->stdout_output;
    if (!cmd_result->stderr_output.empty()) {
        if (!result.output.empty())
            result.output += '\n';
        result.output += cmd_result->stderr_output;
    }

    return result;
}

auto Scheduler::build_job_list(graph::BuildGraph const& graph)
    -> Result<std::vector<BuildJob>>
{
    // Get topological order
    auto topo_result = graph::TopoSortResult{graph::topological_sort(graph)};
    if (topo_result.has_cycle)
        return make_error<std::vector<BuildJob>>(
            ErrorCode::CyclicDependency, "Dependency cycle detected");

    auto jobs = std::vector<BuildJob> {};

    for (auto id : topo_result.order) {
        auto const* node = graph.get_node(id);
        if (!node || node->type != NodeType::Command)
            continue;

        auto job = BuildJob {
            .id = id,
            .command = node->command,
            .display = node->display.empty() ? node->command : node->display,
            .working_dir = options_.root_dir,
            .inputs = {},
            .outputs = {},
        };

        // Collect input paths
        for (auto input_id : graph.get_inputs(id)) {
            if (auto const* input_node = graph.get_node(input_id)) {
                if (!input_node->path.empty())
                    job.inputs.push_back(input_node->path);
            }
        }

        // Collect output paths
        for (auto output_id : graph.get_outputs(id)) {
            if (auto const* output_node = graph.get_node(output_id)) {
                if (!output_node->path.empty())
                    job.outputs.push_back(output_node->path);
            }
        }

        // Collect order-only input paths
        for (auto oi_id : graph.get_order_only(id)) {
            if (auto const* oi_node = graph.get_node(oi_id)) {
                if (!oi_node->path.empty())
                    job.order_only_inputs.push_back(oi_node->path);
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
        if (affected_nodes.count(job.id) > 0)
            result.push_back(job);
    }

    return result;
}

auto detect_parallelism() -> std::size_t
{
    auto hw = std::size_t{std::thread::hardware_concurrency()};
    return hw > 0 ? hw : 1;
}

auto find_changed_files(
    std::filesystem::path const& root,
    index::Index const& old_index) -> std::vector<std::string>
{
    auto changed = std::vector<std::string> {};

    for (auto const& file : old_index.files()) {
        if (file.type != NodeType::File)
            continue;

        auto path = std::filesystem::path{root / file.path};
        struct stat st;

        if (::stat(path.c_str(), &st) < 0) {
            // File was deleted
            changed.push_back(file.path);
            continue;
        }

        auto current_mtime = FileTime {
            .seconds = st.st_mtim.tv_sec,
            .nanoseconds = static_cast<std::int32_t>(st.st_mtim.tv_nsec),
        };

        if (current_mtime != file.mtime) {
            // File was modified
            changed.push_back(file.path);
        }
    }

    return changed;
}

} // namespace pup::exec
