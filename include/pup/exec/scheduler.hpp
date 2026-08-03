// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/function.hpp"
#include "pup/core/node_id_map.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"

#include <chrono>
#include <optional>

namespace pup::graph {
struct BuildGraph;
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

/// One command that must run before another, on evidence the graph does not carry.
/// A discovered dependency is recorded only in the index, so without this the scheduler
/// would order the pair by nothing and they would race on every build that runs both (#276).
struct OrderingEdge {
    NodeId producer = INVALID_NODE_ID;
    NodeId consumer = INVALID_NODE_ID;
};

/// Callback types for scheduler events
using JobStartCallback = Function<void(BuildJob const&)>;
using JobCompleteCallback = Function<void(BuildJob const&, JobResult const&)>;
using ProgressCallback = Function<void(std::size_t completed, std::size_t total)>;

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
    bool injected_ordering_applied = true;     ///< False when the extra ordering contradicted the graph and was dropped
    /// The `ordering` edges this build actually put into the dependency map, and only those. A
    /// pair whose endpoints are not both in the job set orders nothing, so anything reading this
    /// as evidence of what ran first must read what was enforced, never what was offered.
    Vec<OrderingEdge> enforced_ordering = {};
};

/// Build scheduler - executes commands in topological order
class Scheduler final {
public:
    explicit Scheduler(SchedulerOptions options = {});
    ~Scheduler();

    Scheduler(Scheduler const&) = delete;
    auto operator=(Scheduler const&) -> Scheduler& = delete;

    Scheduler(Scheduler&&) noexcept;
    auto operator=(Scheduler&&) noexcept -> Scheduler&;

    /// Build commands matching the filter. nullptr = build all.
    /// `ordering` adds edges the graph does not carry; endpoints outside the filter are ignored.
    [[nodiscard]]
    auto build(
        graph::BuildGraph const& state,
        NodeIdMap32 const* filter = nullptr,
        Vec<OrderingEdge> const& ordering = {}
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

    /// Execute jobs in parallel using the async event loop
    auto execute_parallel(
        Vec<BuildJob> const& jobs,
        graph::BuildGraph const& graph,
        Vec<OrderingEdge> const& ordering
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
