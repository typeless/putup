// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "runner.hpp"
#include "pup/core/result.hpp"
#include "pup/core/types.hpp"
#include "pup/graph/dag.hpp"
#include "pup/index/entry.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace pup::exec {

/// Build job representing a single command to execute
struct BuildJob {
    NodeId id = 0;
    std::string command = {};
    std::string display = {};
    std::filesystem::path working_dir = {};
    std::vector<std::string> inputs = {};
    std::vector<std::string> outputs = {};
};

/// Result of executing a build job
struct JobResult {
    NodeId id = 0;
    bool success = false;
    int exit_code = 0;
    std::string output = {};
    std::chrono::milliseconds duration = {};
};

/// Callback types for scheduler events
using JobStartCallback = std::function<void(BuildJob const&)>;
using JobCompleteCallback = std::function<void(BuildJob const&, JobResult const&)>;
using ProgressCallback = std::function<void(std::size_t completed, std::size_t total)>;

/// Scheduler options
struct SchedulerOptions {
    std::size_t jobs = 0;                        ///< Parallel jobs (0 = auto-detect)
    bool keep_going = false;                     ///< Continue after failures
    bool dry_run = false;                        ///< Print commands without executing
    bool verbose = false;                        ///< Print commands as they run
    std::filesystem::path root_dir = {};         ///< Project root directory
    std::optional<std::chrono::seconds> timeout = {}; ///< Per-command timeout
};

/// Build statistics
struct BuildStats {
    std::size_t total_jobs = 0;
    std::size_t completed_jobs = 0;
    std::size_t failed_jobs = 0;
    std::size_t skipped_jobs = 0;
    std::chrono::milliseconds total_time = {};
    std::chrono::milliseconds build_time = {};   ///< Time spent in commands
};

/// Build scheduler - executes commands in topological order
class Scheduler {
public:
    explicit Scheduler(SchedulerOptions options = {});

    /// Build from a dependency graph
    [[nodiscard]] auto build(graph::BuildGraph const& graph) -> Result<BuildStats>;

    /// Build only the commands that depend on changed files
    [[nodiscard]] auto build_incremental(
        graph::BuildGraph const& graph,
        index::Index const& old_index,
        std::vector<std::string> const& changed_files) -> Result<BuildStats>;

    /// Set callback for job start
    auto on_job_start(JobStartCallback callback) -> void
    {
        on_start_ = std::move(callback);
    }

    /// Set callback for job completion
    auto on_job_complete(JobCompleteCallback callback) -> void
    {
        on_complete_ = std::move(callback);
    }

    /// Set callback for progress updates
    auto on_progress(ProgressCallback callback) -> void
    {
        on_progress_ = std::move(callback);
    }

    /// Request build cancellation
    auto cancel() -> void { cancelled_.store(true); }

    /// Check if build was cancelled
    [[nodiscard]] auto is_cancelled() const -> bool { return cancelled_.load(); }

    /// Get current statistics
    [[nodiscard]] auto stats() const -> BuildStats { return stats_; }

private:
    SchedulerOptions options_ = {};
    BuildStats stats_ = {};
    std::atomic<bool> cancelled_ = false;

    JobStartCallback on_start_ = {};
    JobCompleteCallback on_complete_ = {};
    ProgressCallback on_progress_ = {};

    /// Execute jobs in parallel
    auto execute_parallel(std::vector<BuildJob> const& jobs) -> Result<void>;

    /// Execute a single job
    auto execute_job(BuildJob const& job, CommandRunner& runner) -> JobResult;

    /// Build job list from graph in topological order
    [[nodiscard]] auto build_job_list(graph::BuildGraph const& graph)
        -> Result<std::vector<BuildJob>>;

    /// Determine which jobs need to run based on changes
    [[nodiscard]] auto filter_jobs(
        std::vector<BuildJob> const& all_jobs,
        std::set<NodeId> const& affected_nodes) -> std::vector<BuildJob>;
};

/// Detect number of available CPUs
[[nodiscard]] auto detect_parallelism() -> std::size_t;

/// Find all files that have changed since last build
[[nodiscard]] auto find_changed_files(
    std::filesystem::path const& root,
    index::Index const& old_index) -> std::vector<std::string>;

} // namespace pup::exec
