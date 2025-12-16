// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "pup/core/result.hpp"
#include "pup/core/types.hpp"
#include "pup/graph/dag.hpp"
#include "pup/index/entry.hpp"
#include "runner.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
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
    std::vector<std::string> order_only_inputs = {}; ///< Order-only dependencies
    std::set<std::string> exported_vars = {};        ///< Env vars to export to command

    // For auto-generated rules (from pattern matching)
    bool capture_stdout = false;             ///< Capture stdout for depfile parsing
    bool inject_implicit_deps = false;       ///< Parse stdout as depfile
    NodeId parent_command = INVALID_NODE_ID; ///< Parent command for implicit deps
};

/// Result of executing a build job
struct JobResult {
    NodeId id = 0;
    bool success = false;
    int exit_code = 0;
    std::string output = {};
    std::chrono::milliseconds duration = {};
    std::vector<std::string> discovered_deps = {}; ///< Implicit deps from .d files
    NodeId deps_for_command = INVALID_NODE_ID;     ///< If set, deps belong to this command (not id)
};

/// Callback types for scheduler events
using JobStartCallback = std::function<void(BuildJob const&)>;
using JobCompleteCallback = std::function<void(BuildJob const&, JobResult const&)>;
using ProgressCallback = std::function<void(std::size_t completed, std::size_t total)>;

/// Scheduler options
struct SchedulerOptions {
    std::size_t jobs = 0;                             ///< Parallel jobs (0 = auto-detect)
    bool keep_going = false;                          ///< Continue after failures
    bool dry_run = false;                             ///< Print commands without executing
    bool verbose = false;                             ///< Print commands as they run
    std::filesystem::path source_root = {};           ///< Source tree root (where Tupfile.ini lives)
    std::filesystem::path output_root = {};           ///< Output tree root (where outputs/.pup go)
    std::optional<std::chrono::seconds> timeout = {}; ///< Per-command timeout
};

/// Build statistics
struct BuildStats {
    std::size_t total_jobs = 0;
    std::size_t completed_jobs = 0;
    std::size_t failed_jobs = 0;
    std::size_t skipped_jobs = 0;
    std::chrono::milliseconds total_time = {};
    std::chrono::milliseconds build_time = {}; ///< Time spent in commands
};

/// Build scheduler - executes commands in topological order
class Scheduler {
public:
    explicit Scheduler(SchedulerOptions options = {});
    ~Scheduler();

    Scheduler(Scheduler const&) = delete;
    auto operator=(Scheduler const&) -> Scheduler& = delete;

    Scheduler(Scheduler&&) noexcept;
    auto operator=(Scheduler&&) noexcept -> Scheduler&;

    /// Build from a dependency graph
    [[nodiscard]] auto build(graph::BuildGraph const& graph) -> Result<BuildStats>;

    /// Build only the commands that depend on changed files
    [[nodiscard]] auto build_incremental(
        graph::BuildGraph const& graph,
        index::Index const& old_index,
        std::vector<std::string> const& changed_files) -> Result<BuildStats>;

    /// Set callback for job start
    auto on_job_start(JobStartCallback callback) -> void;

    /// Set callback for job completion
    auto on_job_complete(JobCompleteCallback callback) -> void;

    /// Set callback for progress updates
    auto on_progress(ProgressCallback callback) -> void;

    /// Request build cancellation
    auto cancel() -> void;

    /// Check if build was cancelled
    [[nodiscard]] auto is_cancelled() const -> bool;

    /// Get current statistics
    [[nodiscard]] auto stats() const -> BuildStats;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;

    /// Execute jobs in parallel
    auto execute_parallel(std::vector<BuildJob> const& jobs, graph::BuildGraph const& graph) -> Result<void>;

    /// Execute a single job
    auto execute_job(BuildJob const& job, CommandRunner& runner,
        std::unordered_map<std::string, std::string> const& env_cache) -> JobResult;

    /// Build job list from graph in topological order
    [[nodiscard]] auto build_job_list(
        graph::BuildGraph const& graph) -> Result<std::vector<BuildJob>>;

    /// Determine which jobs need to run based on changes
    [[nodiscard]] auto filter_jobs(
        std::vector<BuildJob> const& all_jobs,
        std::set<NodeId> const& affected_nodes) -> std::vector<BuildJob>;
};

/// Detect number of available CPUs
[[nodiscard]] auto detect_parallelism() -> std::size_t;

} // namespace pup::exec
