// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/node_id_map.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "runner.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace pup::graph {
class BuildGraph;
}

namespace pup::exec {

/// Build job representing a single command to execute
struct BuildJob {
    NodeId id = 0;
    StringId command = StringId::Empty;
    StringId display = StringId::Empty;
    StringId working_dir = StringId::Empty;
    Vec<StringId> inputs = {};
    Vec<StringId> outputs = {};
    Vec<StringId> order_only_inputs = {}; ///< Order-only dependencies
    Vec<StringId> exported_vars = {};     ///< Env vars to export to command

    // For auto-generated rules (from pattern matching)
    bool capture_stdout = false;             ///< Capture stdout for depfile parsing
    bool inject_implicit_deps = false;       ///< Parse stdout as depfile
    NodeId parent_command = INVALID_NODE_ID; ///< Parent command for implicit deps

    // For phi-node model: true if all guards are satisfied
    bool guard_active = true;
};

/// Result of executing a build job
struct JobResult {
    NodeId id = 0;
    bool success = false;
    int exit_code = 0;
    StringId output = StringId::Empty;
    std::chrono::milliseconds duration = {};
    Vec<StringId> discovered_deps = {};        ///< Implicit deps from .d files
    NodeId deps_for_command = INVALID_NODE_ID; ///< If set, deps belong to this command (not id)
};

/// Callback types for scheduler events
using JobStartCallback = std::function<void(BuildJob const&)>;
using JobCompleteCallback = std::function<void(BuildJob const&, JobResult const&)>;
using ProgressCallback = std::function<void(std::size_t completed, std::size_t total)>;

/// Scheduler options
struct SchedulerOptions {
    std::size_t jobs = 0;    ///< Parallel jobs (0 = auto-detect)
    bool keep_going = false; ///< Continue after failures
    bool dry_run = false;    ///< Print commands without executing
    bool verbose = false;    ///< Print commands as they run
    StringId source_root = StringId::Empty;
    StringId config_root = StringId::Empty;
    StringId output_root = StringId::Empty;           ///< Output tree root (where outputs/.pup go)
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
    [[nodiscard]]
    auto build(graph::BuildGraph const& graph) -> Result<BuildStats>;

    /// Build only the commands that depend on changed files
    [[nodiscard]]
    auto build_incremental(
        graph::BuildGraph const& graph,
        Vec<StringId> const& changed_files
    ) -> Result<BuildStats>;

    /// Build only a specific subset of commands
    [[nodiscard]]
    auto build_subset(
        graph::BuildGraph const& graph,
        NodeIdMap32 const& command_ids
    ) -> Result<BuildStats>;

    /// Build specific targets and all required dependencies.
    /// Uses reverse traversal to find commands needed.
    [[nodiscard]]
    auto build_targets(
        graph::BuildGraph const& graph,
        Vec<NodeId> const& target_ids
    ) -> Result<BuildStats>;

    /// Set callback for job start
    auto on_job_start(JobStartCallback callback) -> void;

    /// Set callback for job completion
    auto on_job_complete(JobCompleteCallback callback) -> void;

    /// Set callback for progress updates
    auto on_progress(ProgressCallback callback) -> void;

    /// Request build cancellation
    auto cancel() -> void;

    /// Check if build was cancelled
    [[nodiscard]]
    auto is_cancelled() const -> bool;

    /// Get current statistics
    [[nodiscard]]
    auto stats() const -> BuildStats;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;

    /// Execute jobs in parallel (or dispatch to sequential for jobs == 1)
    auto execute_parallel(
        Vec<BuildJob> const& jobs,
        graph::BuildGraph const& graph
    ) -> Result<void>;

    /// Build job list from graph in topological order
    [[nodiscard]]
    auto build_job_list(
        graph::BuildGraph const& graph
    ) -> Result<Vec<BuildJob>>;

    /// Determine which jobs need to run based on changes
    [[nodiscard]]
    auto filter_jobs(
        Vec<BuildJob> const& all_jobs,
        NodeIdMap32 const& affected_nodes
    ) -> Vec<BuildJob>;
};

/// Detect number of available CPUs
[[nodiscard]]
auto detect_parallelism() -> std::size_t;

} // namespace pup::exec
