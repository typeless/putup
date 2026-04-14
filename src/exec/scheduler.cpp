// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/exec/scheduler.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/clock.hpp"
#include "pup/core/expected.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/heap_buf.hpp"
#include "pup/core/metrics.hpp"
#include "pup/core/node_id_map.hpp"
#include "pup/core/path.hpp"
#include "pup/core/platform.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/rule_pattern.hpp"
#include "pup/graph/topo.hpp"
#include "pup/parser/depfile.hpp"
#include "pup/platform/file_io.hpp"
#include "pup/platform/process.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <queue>
#include <string_view>
#include <utility>

namespace pup::exec {

namespace {

using EnvCache = Vec<std::pair<StringId, StringId>>;

auto env_cache_find(EnvCache const& cache, std::string_view key) -> std::pair<StringId, StringId> const*
{
    auto& pool = global_pool();
    auto pos = std::lower_bound(cache.begin(), cache.end(), key, [&pool](auto const& p, std::string_view k) { return pool.get(p.first) < k; });
    return (pos != cache.end() && pool.get(pos->first) == key) ? pos : nullptr;
}

/// Build immutable sorted cache of environment variables for exported vars.
/// Must be called before spawning worker threads (getenv is not thread-safe).
auto build_env_cache(Vec<BuildJob> const& jobs) -> EnvCache
{
    auto& pool = global_pool();

    // Collect unique var names
    auto names = Vec<StringId> {};
    for (auto const& job : jobs) {
        for (auto var_id : job.exported_vars) {
            names.push_back(var_id);
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    // Look up each name once
    auto cache = EnvCache {};
    cache.reserve(names.size());
    auto namebuf = Buf {};
    for (auto name_id : names) {
        namebuf.clear();
        namebuf.append(pool.get(name_id));
        if (auto const* env_val = std::getenv(namebuf.c_str())) {
            cache.emplace_back(name_id, pool.intern(env_val));
        }
    }
    return cache;
}

/// Resolve an output path to a filesystem path, handling variant-mapped paths.
/// If the path is already prefixed with the variant output directory, use source_root as base.
/// Otherwise, use output_root as base.
auto resolve_variant_path(
    std::string_view source_root,
    std::string_view output_root,
    std::string_view output_root_prefix,
    std::string_view path
) -> std::string_view
{
    auto& pool = pup::global_pool();
    if (!output_root_prefix.empty() && path.starts_with(output_root_prefix)
        && (path.size() == output_root_prefix.size() || path[output_root_prefix.size()] == '/')) {
        return pool.get(pup::path::join(source_root, path));
    }
    return pool.get(pup::path::join(output_root, path));
}

struct PreparedJob {
    Vec<StringId> env_ids;
    StringId working_dir;
};

auto prepare_job_launch(
    BuildJob const& job,
    EnvCache const& env_cache,
    SchedulerOptions const& options,
    std::string_view source_root_sv,
    std::string_view output_root_sv,
    std::string_view output_root_prefix
) -> PreparedJob
{
    auto& pool = global_pool();

    for (auto output_id : job.outputs) {
        auto output_sv = pool.get(output_id);
        auto output_path = pup::path::is_absolute(output_sv)
            ? output_sv
            : resolve_variant_path(source_root_sv, output_root_sv, output_root_prefix, output_sv);
        auto parent = pup::path::parent(output_path);
        if (!parent.empty()) {
            (void)pup::platform::create_directories(parent);
        }
    }

    auto env_ids = Vec<StringId> {};
    for (auto var_id : job.exported_vars) {
        auto var_sv = pool.get(var_id);
        if (auto it = env_cache_find(env_cache, var_sv)) {
            auto eb = Buf {};
            eb.append(var_sv);
            eb.append('=');
            eb.append(pool.get(it->second));
            env_ids.push_back(eb.intern(pool));
        }
    }

    auto working_dir = job.working_dir;
    if (is_empty(working_dir)) {
        working_dir = options.source_root;
    }

    return { std::move(env_ids), working_dir };
}

auto parse_stdout_depfile(
    BuildJob const& job,
    std::string_view stdout_sv,
    Vec<StringId>& out_deps,
    NodeId& out_deps_cmd
) -> void
{
    if (job.inject_implicit_deps && !stdout_sv.empty()) {
        auto depfile_result = parser::parse_depfile(stdout_sv);
        if (depfile_result) {
            for (auto dep_id : depfile_result->dependencies) {
                out_deps.push_back(dep_id);
            }
            out_deps_cmd = job.parent_command;
        }
    }
}

auto discover_d_file_deps(
    BuildJob const& job,
    JobResult& result,
    std::string_view source_root_sv,
    std::string_view output_root_sv,
    std::string_view output_root_prefix
) -> void
{
    auto& pool = global_pool();

    for (auto output_id : job.outputs) {
        auto output_sv = pool.get(output_id);
        auto ext = pup::path::extension(output_sv);
        if (ext != ".o" && ext != ".obj") {
            continue;
        }
        auto base_path = resolve_variant_path(source_root_sv, output_root_sv, output_root_prefix, pup::path::parent(output_sv));
        auto stem_buf = Buf {};
        stem_buf.append(pup::path::stem(output_sv));
        stem_buf.append(".d");
        auto depfile_path = pool.get(pup::path::join(base_path, stem_buf.view()));
        if (!pup::platform::exists(depfile_path)) {
            continue;
        }
        auto depfile_result = parser::parse_depfile_path(depfile_path);
        if (depfile_result) {
            for (auto dep_id : depfile_result->dependencies) {
                result.discovered_deps.push_back(dep_id);
            }
            if (result.deps_for_command == INVALID_NODE_ID) {
                result.deps_for_command = job.id;
            }
        }
    }
}

/// Add job dependencies for any command that produces the given node.
/// For phi-nodes (multiple producers), only add active producers when consumer is active.
auto sorted_insert_unique(Vec<std::size_t>& v, std::size_t val) -> void
{
    auto pos = std::lower_bound(v.begin(), v.end(), val);
    if (pos == v.end() || *pos != val) {
        v.insert(pos, val);
    }
}

auto add_producer_dependencies(
    graph::Graph const& graph,
    NodeIdMap32 const& cmd_to_job,
    Vec<BuildJob> const& jobs,
    NodeId node_id,
    std::size_t current_job,
    Vec<std::size_t>& dependencies
) -> void
{
    auto current_active = jobs[current_job].guard_active;

    for (auto producer_id : graph::get_inputs(graph, node_id)) {
        if (node_id::is_command(producer_id) && cmd_to_job.contains(producer_id)) {
            auto dep_idx = static_cast<std::size_t>(cmd_to_job.get(producer_id));
            if (dep_idx != current_job) {
                if (!current_active || jobs[dep_idx].guard_active) {
                    sorted_insert_unique(dependencies, dep_idx);
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
    Vec<BuildJob> const& jobs,
    graph::Graph const& graph
) -> std::pair<Vec<std::size_t>, Vec<Vec<std::size_t>>>
{
    auto in_degree = Vec<std::size_t> {};
    in_degree.resize(jobs.size());
    auto dependents = Vec<Vec<std::size_t>> {};
    dependents.resize(jobs.size());

    // Build map from command NodeId -> job index
    auto cmd_to_job = NodeIdMap32 {};
    for (auto i = std::size_t { 0 }; i < jobs.size(); ++i) {
        cmd_to_job.set(jobs[i].id, static_cast<std::uint32_t>(i));
    }

    // For each job, find dependencies via input edges
    for (auto j = std::size_t { 0 }; j < jobs.size(); ++j) {
        auto dependencies = Vec<std::size_t> {};
        auto cmd_id = jobs[j].id;

        auto current_active = jobs[j].guard_active;

        // Check regular inputs - traverse graph edges
        for (auto input_id : graph::get_inputs(graph, cmd_id)) {
            // Case 1: Input itself is a command (e.g., generated dep-scan rule)
            if (node_id::is_command(input_id)) {
                if (cmd_to_job.contains(input_id)) {
                    auto dep_idx = static_cast<std::size_t>(cmd_to_job.get(input_id));
                    if (dep_idx != j) {
                        if (!current_active || jobs[dep_idx].guard_active) {
                            sorted_insert_unique(dependencies, dep_idx);
                        }
                    }
                }
                continue;
            }

            // Case 2: Input is a file produced by another command
            add_producer_dependencies(graph, cmd_to_job, jobs, input_id, j, dependencies);
        }

        // Case 3: Order-only inputs (groups and files)
        // These establish ordering without creating true data dependencies.
        for (auto oo_id : graph::get_order_only(graph, cmd_id)) {
            // For Group nodes, get member files and find their producers
            if (graph::get<NodeType>(graph, oo_id) == NodeType::Group) {
                for (auto member_id : graph::get_inputs(graph, oo_id)) {
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
    Vec<BuildJob> const& jobs,
    Vec<Vec<std::size_t>> const& dependents
) -> Result<void>
{
    for (auto i = std::size_t { 0 }; i < jobs.size(); ++i) {
        if (jobs[i].guard_active) {
            continue;
        }
        // This job is skipped - check if any active job depends on it
        for (auto dep_idx : dependents[i]) {
            if (jobs[dep_idx].guard_active) {
                auto& pool = global_pool();
                auto eb = Buf {};
                eb.append("Command '");
                eb.append(pool.get(jobs[dep_idx].command));
                eb.append("' depends on output from '");
                eb.append(pool.get(jobs[i].command));
                eb.append("' which is inactive due to conditional guard");
                return make_error<void>(ErrorCode::MissingInput, eb.view());
            }
        }
    }
    return {};
}

struct JobSlot {
    pup::platform::AsyncProcess process = {};
    HeapBuf stdout_buf = {};
    HeapBuf stderr_buf = {};
    std::size_t job_index = 0;
    pup::SteadyClock::time_point start_time = {};

    auto active() const -> bool { return process.active(); }

    auto reset() -> void
    {
        process = {};
        stdout_buf.clear();
        stderr_buf.clear();
        job_index = 0;
    }
};

auto launch_job(
    JobSlot& slot,
    BuildJob const& job,
    std::size_t job_idx,
    StringId working_dir,
    Vec<StringId> const& env_ids,
    bool inherit_env
) -> bool
{
    auto& pool = global_pool();

    auto command_str = pool.get(job.command);
    auto working_dir_str = pool.get(working_dir);

    auto env_strings = pup::platform::build_env_strings(env_ids, inherit_env);
    auto env_c_strs = Vec<char*> {};
    env_c_strs.reserve(env_strings.size() + 1);
    for (auto s : env_strings) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) - POSIX exec requires char*
        env_c_strs.push_back(const_cast<char*>(pool.get(s).data()));
    }
    env_c_strs.push_back(nullptr);

    auto opts = pup::platform::SpawnOptions {
        .command = command_str,
        .working_dir = working_dir_str,
        .env = (inherit_env && env_ids.empty()) ? nullptr : env_c_strs.data(),
    };

    auto result = pup::platform::spawn_async(opts);
    if (!result) {
        return false;
    }

    slot.process = *result;
    slot.job_index = job_idx;
    slot.start_time = pup::SteadyClock::now();

    return true;
}

auto reap_slot(JobSlot& slot, pup::platform::ProcessStatus const& status) -> JobResult
{
    auto& pool = global_pool();
    auto result = JobResult {};
    result.id = 0;

    result.exit_code = status.exit_code;
    result.success = (status.exit_code == 0);

    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        pup::SteadyClock::now() - slot.start_time
    );

    auto output_buf = HeapBuf {};
    if (!slot.stdout_buf.empty()) {
        output_buf.append(slot.stdout_buf.view());
    }
    if (!slot.stderr_buf.empty()) {
        if (!output_buf.empty()) {
            output_buf.append('\n');
        }
        output_buf.append(slot.stderr_buf.view());
    }
    if (!output_buf.empty()) {
        result.output = pool.intern(output_buf.view());
    }

    if (slot.process.stdout_fd != -1) {
        pup::platform::close_fd(slot.process.stdout_fd);
    }
    if (slot.process.stderr_fd != -1) {
        pup::platform::close_fd(slot.process.stderr_fd);
    }

    slot.reset();
    return result;
}

auto kill_slot(JobSlot& slot) -> void
{
    if (slot.active()) {
        pup::platform::send_signal(slot.process.pid, pup::platform::Signal::Terminate);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

struct Scheduler::Impl {
    SchedulerOptions options;
    BuildStats stats;
    bool cancelled = false;

    JobStartCallback on_start;
    JobCompleteCallback on_complete;
    ProgressCallback on_progress;
};

Scheduler::Scheduler(SchedulerOptions options)
    : impl_(std::make_unique<Impl>())
{
    impl_->options = std::move(options);
    if (impl_->options.jobs == 0) {
        impl_->options.jobs = pup::cpu_count();
    }
}

Scheduler::~Scheduler() = default;

Scheduler::Scheduler(Scheduler&&) noexcept = default;

auto Scheduler::operator=(Scheduler&&) noexcept -> Scheduler& = default;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

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
    impl_->cancelled = true;
}

auto Scheduler::is_cancelled() const -> bool
{
    return impl_->cancelled;
}

auto Scheduler::stats() const -> BuildStats
{
    return impl_->stats;
}

// ---------------------------------------------------------------------------
// Public build API
// ---------------------------------------------------------------------------

auto Scheduler::build(graph::BuildGraph const& state, NodeIdMap32 const* filter) -> Result<BuildStats>
{
    impl_->cancelled = false;
    impl_->stats = BuildStats {};

    auto start_time = pup::SteadyClock::time_point { pup::SteadyClock::now() };

    auto all_jobs = build_job_list(state);
    if (!all_jobs) {
        return pup::unexpected<Error>(all_jobs.error());
    }

    auto jobs = Vec<BuildJob> {};
    if (filter) {
        jobs = filter_jobs(*all_jobs, *filter);
        impl_->stats.skipped_jobs = all_jobs->size() - jobs.size();
    } else {
        jobs = std::move(*all_jobs);
    }

    impl_->stats.total_jobs = jobs.size();

    if (jobs.empty()) {
        impl_->stats.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            pup::SteadyClock::now() - start_time
        );
        return impl_->stats;
    }

    auto exec_result = execute_parallel(jobs, state);

    impl_->stats.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        pup::SteadyClock::now() - start_time
    );

    if (!exec_result && !impl_->options.keep_going) {
        return pup::unexpected<Error>(exec_result.error());
    }

    return impl_->stats;
}

auto Scheduler::filter_jobs(
    Vec<BuildJob> const& all_jobs,
    NodeIdMap32 const& affected_nodes
) -> Vec<BuildJob>
{
    auto result = Vec<BuildJob> {};
    result.reserve(all_jobs.size());

    for (auto const& job : all_jobs) {
        if (affected_nodes.contains(job.id)) {
            result.push_back(job);
        }
    }

    return result;
}

auto Scheduler::build_job_list(
    graph::BuildGraph const& state
) -> Result<Vec<BuildJob>>
{
    auto job_list_start = pup::SteadyClock::now();

    // Get topological order
    auto topo_result = graph::TopoSortResult { graph::topological_sort(state.graph) };
    if (topo_result.has_cycle) {
        return make_error<Vec<BuildJob>>(
            ErrorCode::CyclicDependency, "Dependency cycle detected"
        );
    }

    auto& pool = global_pool();
    auto output_root_sv = pool.get(impl_->options.output_root);
    auto source_root_sv = pool.get(impl_->options.source_root);
    auto config_root_sv = pool.get(impl_->options.config_root);

    // Validate no unrealized ghost nodes remain (missing inputs)
    // Exception: Ghost nodes whose files actually exist on disk are valid
    // non-generated input files (e.g., tup.config, manually-created config files)
    auto const& g = state.graph;
    auto& cache = state.path_cache;

    for (auto id : topo_result.order) {
        if (graph::get<NodeType>(g, id) == NodeType::Ghost && !graph::get_outputs(g, id).empty()) {
            auto path_sv = graph::get_full_path(g, id, cache);
            auto build_root_name = graph::get_build_root_name(g);
            auto file_path_sv = pool.get(pup::path::join(output_root_sv, path_sv));
            auto lookup_path = path_sv;
            auto build_prefix = Buf {};
            build_prefix += build_root_name;
            build_prefix += '/';
            if (!build_root_name.empty() && path_sv.starts_with(build_prefix.view())) {
                lookup_path = path_sv.substr(build_prefix.size());
                file_path_sv = pool.get(pup::path::join(output_root_sv, lookup_path));
            }
            if (pup::platform::exists(file_path_sv)) {
                continue;
            }
            auto err = Buf {};
            err.fmt("Missing input file (unresolved ghost): {}\n  Hint: try building with -a to include upstream dependencies", path_sv);
            return make_error<Vec<BuildJob>>(ErrorCode::ParseError, err.view());
        }
    }

    auto jobs = Vec<BuildJob> {};

    for (auto id : topo_result.order) {
        if (!node_id::is_command(id)) {
            continue;
        }
        // Compute working directory: source_dir for subdirectory Tupfiles.
        // Commands run from the Tupfile's SOURCE directory so that relative paths
        // and TUP_VARIANT_OUTPUTDIR work correctly. Output paths are already
        // mapped to the output directory by the builder.
        auto source_dir = graph::get_source_dir(g, id);
        auto working_dir_id = StringId::Empty;
        if (!source_dir.empty()) {
            working_dir_id = pup::path::join(source_root_sv, source_dir);
        } else {
            working_dir_id = pool.intern(source_root_sv);
        }

        // Check if this is a generated rule that captures stdout
        auto capture_stdout = graph::is_stdout_capture(g, id);
        auto inject_implicit = capture_stdout && graph::get<graph::OutputAction>(g, id) == graph::OutputAction::InjectImplicitDeps;
        auto parent_cmd = inject_implicit ? graph::get_parent_command(g, id) : INVALID_NODE_ID;

        // Expand command from instruction pattern + operands
        auto cmd_id = graph::expand_instruction(g, id, cache, source_root_sv, config_root_sv);
        auto display_id = graph::get_display_id(g, id);

        auto const& exported_raw = graph::get_exported_vars(g, id);
        auto exported_ids = Vec<StringId> {};
        exported_ids.reserve(exported_raw.size());
        for (auto raw_id : exported_raw) {
            exported_ids.push_back(make_string_id(raw_id));
        }

        // Evaluate guards - command only executes if ALL guards are satisfied
        auto guard_active = graph::is_guard_satisfied(g, id);

        auto job = BuildJob {
            .id = id,
            .command = cmd_id,
            .display = is_empty(display_id) ? cmd_id : display_id,
            .working_dir = working_dir_id,
            .inputs = {},
            .outputs = {},
            .order_only_inputs = {},
            .exported_vars = std::move(exported_ids),
            .capture_stdout = capture_stdout,
            .inject_implicit_deps = inject_implicit,
            .parent_command = parent_cmd,
            .guard_active = guard_active,
        };

        // Collect input paths
        for (auto input_id : graph::get_inputs(g, id)) {
            auto input_path = graph::get_full_path(g, input_id, cache);
            if (!input_path.empty()) {
                job.inputs.push_back(pool.intern(input_path));
            }
        }

        // Collect output paths
        for (auto output_id : graph::get_outputs(g, id)) {
            auto output_path = graph::get_full_path(g, output_id, cache);
            if (!output_path.empty()) {
                job.outputs.push_back(pool.intern(output_path));
            }
        }

        // Collect order-only input paths
        // For Group nodes, expand to member file paths
        for (auto oi_id : graph::get_order_only(g, id)) {
            if (graph::get<NodeType>(g, oi_id) == NodeType::Group) {
                for (auto member_id : graph::get_inputs(g, oi_id)) {
                    auto member_path = graph::get_full_path(g, member_id, cache);
                    if (!member_path.empty()) {
                        job.order_only_inputs.push_back(pool.intern(member_path));
                    }
                }
            } else {
                auto oi_path = graph::get_full_path(g, oi_id, cache);
                if (!oi_path.empty()) {
                    job.order_only_inputs.push_back(pool.intern(oi_path));
                }
            }
        }

        jobs.push_back(std::move(job));
    }

    auto job_list_elapsed = pup::SteadyClock::now() - job_list_start;
    thread_metrics().job_list_time += std::chrono::duration_cast<std::chrono::microseconds>(job_list_elapsed);

    return jobs;
}

auto Scheduler::execute_parallel(
    Vec<BuildJob> const& jobs,
    graph::BuildGraph const& state
) -> Result<void>
{
    auto const env_cache = build_env_cache(jobs);

    auto& pool = global_pool();

    // Build dependency map
    auto [in_degree, dependents] = build_dependency_map(jobs, state.graph);

    if (auto result = validate_guard_dependencies(jobs, dependents); !result) {
        return pup::unexpected<Error>(result.error());
    }

    auto active_count = static_cast<std::size_t>(
        std::count_if(jobs.begin(), jobs.end(), [](auto const& j) { return j.guard_active; })
    );
    impl_->stats.skipped_jobs += jobs.size() - active_count;

    if (active_count == 0) {
        return {};
    }

    auto ready_queue = std::queue<std::size_t> {};
    for (auto i = std::size_t { 0 }; i < jobs.size(); ++i) {
        if (in_degree[i] == 0 && jobs[i].guard_active) {
            ready_queue.push(i);
        }
    }

    // Job slots -- one per concurrent child process.
    // JobSlot is non-copyable/non-movable (HeapBuf), so allocate via new[].
    auto max_jobs = std::min(impl_->options.jobs, active_count);
    auto slots_storage = std::unique_ptr<JobSlot[]>(new JobSlot[max_jobs]); // NOLINT
    auto* slots = slots_storage.get();

    auto running = std::size_t { 0 };
    auto finished_count = std::size_t { 0 };
    auto failed = false;

    // Pre-compute source/output roots for job preparation
    auto source_root_sv = pool.get(impl_->options.source_root);
    auto output_root_sv = pool.get(impl_->options.output_root);
    auto output_root_prefix = pool.get(pup::path::relative(output_root_sv, source_root_sv));

    while (finished_count < active_count) {
        // 1. Launch: fill empty slots from ready_queue
        if (!failed || impl_->options.keep_going) {
            for (auto s = std::size_t { 0 }; s < max_jobs; ++s) {
                auto& slot = slots[s];
                if (slot.active() || ready_queue.empty()) {
                    continue;
                }

                auto job_idx = ready_queue.front();
                auto const& job = jobs[job_idx];

                auto prepared = prepare_job_launch(job, env_cache, impl_->options, source_root_sv, output_root_sv, output_root_prefix);

                if (impl_->options.dry_run) {
                    // Dry run: don't fork, just report success
                    ready_queue.pop();

                    if (impl_->on_start) {
                        impl_->on_start(job);
                    }

                    auto result = JobResult {
                        .id = job.id,
                        .success = true,
                    };

                    if (impl_->on_complete) {
                        impl_->on_complete(job, result);
                    }

                    ++impl_->stats.completed_jobs;
                    ++finished_count;
                    impl_->stats.build_time += result.duration;

                    for (auto dep_idx : dependents[job_idx]) {
                        if (--in_degree[dep_idx] == 0 && jobs[dep_idx].guard_active) {
                            ready_queue.push(dep_idx);
                        }
                    }

                    if (impl_->on_progress) {
                        impl_->on_progress(finished_count, impl_->stats.total_jobs);
                    }
                    continue;
                }

                if (!launch_job(slot, job, job_idx, prepared.working_dir, prepared.env_ids, true)) {
                    // Fork failed -- treat as job failure
                    ready_queue.pop();

                    if (impl_->on_start) {
                        impl_->on_start(job);
                    }

                    auto result = JobResult {
                        .id = job.id,
                        .success = false,
                        .output = pool.intern("Failed to launch process"),
                    };

                    if (impl_->on_complete) {
                        impl_->on_complete(job, result);
                    }

                    ++impl_->stats.failed_jobs;
                    ++finished_count;
                    failed = true;

                    if (impl_->on_progress) {
                        impl_->on_progress(finished_count, impl_->stats.total_jobs);
                    }
                    continue;
                }

                ready_queue.pop();
                ++running;

                if (impl_->on_start) {
                    impl_->on_start(job);
                }
            }
        }

        // If nothing is running and queue is empty, we're done
        // (remaining jobs are blocked by failed dependencies)
        if (running == 0 && ready_queue.empty()) {
            break;
        }

        // 2. Build pollable FD array from active slots
        auto pfds = Vec<pup::platform::PollableFd> {};
        pfds.reserve(running * 2);

        for (auto i = std::size_t { 0 }; i < max_jobs; ++i) {
            if (!slots[i].active()) {
                continue;
            }
            if (slots[i].process.stdout_fd != -1) {
                pfds.push_back({ .fd = slots[i].process.stdout_fd, .is_stderr = false, .slot_index = i });
            }
            if (slots[i].process.stderr_fd != -1) {
                pfds.push_back({ .fd = slots[i].process.stderr_fd, .is_stderr = true, .slot_index = i });
            }
        }

        // Compute poll timeout from per-job timeouts
        auto poll_timeout = -1;
        if (impl_->options.timeout) {
            auto now = pup::SteadyClock::now();
            auto min_remaining = std::chrono::milliseconds::max();
            for (auto si = std::size_t { 0 }; si < max_jobs; ++si) {
                if (!slots[si].active()) {
                    continue;
                }
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - slots[si].start_time);
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*impl_->options.timeout) - elapsed;
                if (remaining < min_remaining) {
                    min_remaining = remaining;
                }
            }
            poll_timeout = std::max(static_cast<int>(min_remaining.count()), 0);
        }

        if (pfds.empty()) {
            poll_timeout = 100; // 100ms fallback for reap
        }

        // 3. Wait for I/O
        auto poll_result = pup::platform::poll_fds(pfds.data(), pfds.size(), poll_timeout);

        // 4. Drain ready pipes
        if (poll_result > 0) {
            char buf[4096]; // NOLINT(modernize-avoid-c-arrays)
            for (auto& pfd : pfds) {
                if (pfd.fd == -1) {
                    continue; // marked not-ready by poll_fds
                }
                auto n = pup::platform::read_nonblocking(pfd.fd, buf, sizeof(buf));
                if (n > 0) {
                    auto& slot = slots[pfd.slot_index];
                    auto data = std::string_view { buf, static_cast<std::size_t>(n) };
                    if (pfd.is_stderr) {
                        slot.stderr_buf.append(data);
                    } else {
                        slot.stdout_buf.append(data);
                    }
                } else if (n == 0) {
                    // EOF on this FD -- close it so we don't poll it again
                    auto& slot = slots[pfd.slot_index];
                    if (pfd.is_stderr) {
                        pup::platform::close_fd(slot.process.stderr_fd);
                        slot.process.stderr_fd = -1;
                    } else {
                        pup::platform::close_fd(slot.process.stdout_fd);
                        slot.process.stdout_fd = -1;
                    }
                }
            }
        }

        // 5. Reap finished children
        for (auto si = std::size_t { 0 }; si < max_jobs; ++si) {
            auto& slot = slots[si];
            if (!slot.active()) {
                continue;
            }

            // Check for timeout
            if (impl_->options.timeout) {
                auto elapsed = pup::SteadyClock::now() - slot.start_time;
                if (elapsed >= *impl_->options.timeout) {
                    pup::platform::send_signal(slot.process.pid, pup::platform::Signal::Kill);
                }
            }

            auto status = pup::platform::ProcessStatus {};
            if (!pup::platform::try_reap(slot.process.pid, status)) {
                continue; // Still running
            }

            // Drain any remaining pipe data before closing
            char buf[4096]; // NOLINT(modernize-avoid-c-arrays)
            if (slot.process.stdout_fd != -1) {
                while (true) {
                    auto n = pup::platform::read_nonblocking(slot.process.stdout_fd, buf, sizeof(buf));
                    if (n > 0) {
                        slot.stdout_buf.append(std::string_view { buf, static_cast<std::size_t>(n) });
                    } else {
                        break;
                    }
                }
            }
            if (slot.process.stderr_fd != -1) {
                while (true) {
                    auto n = pup::platform::read_nonblocking(slot.process.stderr_fd, buf, sizeof(buf));
                    if (n > 0) {
                        slot.stderr_buf.append(std::string_view { buf, static_cast<std::size_t>(n) });
                    } else {
                        break;
                    }
                }
            }

            auto job_idx = slot.job_index;
            auto const& job = jobs[job_idx];

            // Parse depfile from stdout BEFORE reap clears the buffer
            auto stdout_deps = Vec<StringId> {};
            auto stdout_deps_cmd = INVALID_NODE_ID;
            parse_stdout_depfile(job, slot.stdout_buf.view(), stdout_deps, stdout_deps_cmd);

            auto result = reap_slot(slot, status);
            result.id = job.id;
            --running;

            if (!stdout_deps.empty()) {
                for (auto dep_id : stdout_deps) {
                    result.discovered_deps.push_back(dep_id);
                }
                result.deps_for_command = stdout_deps_cmd;
            }

            if (result.success) {
                discover_d_file_deps(job, result, source_root_sv, output_root_sv, output_root_prefix);
            }

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
                failed = true;
            }

            ++finished_count;

            if (impl_->on_progress) {
                impl_->on_progress(finished_count, impl_->stats.total_jobs);
            }
        }

        // 6. Check termination
        if (impl_->cancelled || (failed && !impl_->options.keep_going)) {
            // Kill remaining children
            for (auto si = std::size_t { 0 }; si < max_jobs; ++si) {
                kill_slot(slots[si]);
            }
            // Give children a moment to exit gracefully, escalate to SIGKILL
            for (auto si = std::size_t { 0 }; si < max_jobs; ++si) {
                if (slots[si].active()) {
                    auto ps = pup::platform::ProcessStatus {};
                    if (!pup::platform::try_reap(slots[si].process.pid, ps)) {
                        pup::platform::send_signal(slots[si].process.pid, pup::platform::Signal::Kill);
                        pup::platform::reap(slots[si].process.pid, ps);
                    }
                    if (slots[si].process.stdout_fd != -1) {
                        pup::platform::close_fd(slots[si].process.stdout_fd);
                    }
                    if (slots[si].process.stderr_fd != -1) {
                        pup::platform::close_fd(slots[si].process.stderr_fd);
                    }
                    slots[si].reset();
                    --running;
                }
            }
            break;
        }
    }

    if (failed && !impl_->options.keep_going) {
        return make_error<void>(ErrorCode::CommandFailed, "Build failed");
    }

    return {};
}

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

auto detect_parallelism() -> std::size_t
{
    return pup::cpu_count();
}

} // namespace pup::exec
