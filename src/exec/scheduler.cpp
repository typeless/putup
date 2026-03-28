// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/exec/scheduler.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/clock.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/heap_buf.hpp"
#include "pup/core/metrics.hpp"
#include "pup/core/node_id_map.hpp"
#include "pup/core/path.hpp"
#include "pup/core/platform.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/graph/dag.hpp"
#include "pup/graph/rule_pattern.hpp"
#include "pup/graph/topo.hpp"
#include "pup/parser/depfile.hpp"
#include "pup/platform/file_io.hpp"
#include "pup/platform/process.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <queue>

#ifndef _WIN32
#    include <cerrno>
#    include <csignal>
#    include <fcntl.h>
#    include <poll.h>
#    include <sys/wait.h>
#    include <unistd.h>
#    ifdef __APPLE__
#        include <crt_externs.h>
#        define environ (*_NSGetEnviron())
#    else
extern "C" char** environ; // NOLINT
#    endif
#endif

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
    graph::BuildGraph const& graph,
    NodeIdMap32 const& cmd_to_job,
    Vec<BuildJob> const& jobs,
    NodeId node_id,
    std::size_t current_job,
    Vec<std::size_t>& dependencies
) -> void
{
    auto current_active = jobs[current_job].guard_active;

    for (auto producer_id : graph.get_inputs(node_id)) {
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
    graph::BuildGraph const& graph
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
        for (auto input_id : graph.get_inputs(cmd_id)) {
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

/// Collect all commands required to build the given target nodes.
/// Uses reverse traversal: starts at targets, walks backward through inputs.
auto collect_required_commands(
    graph::BuildGraph const& graph,
    Vec<NodeId> const& target_ids
) -> NodeIdMap32
{
    auto visited = NodeIdMap32 {};
    auto commands = NodeIdMap32 {};
    auto stack = Vec<NodeId> {};
    for (auto id : target_ids) {
        stack.push_back(id);
    }

    while (!stack.empty()) {
        auto id = stack.back();
        stack.pop_back();

        if (visited.contains(id)) {
            continue;
        }
        visited.set(id, 1);

        if (node_id::is_command(id) && graph.get_command_node(id)) {
            commands.set(id, 1);
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

#ifndef _WIN32

struct JobSlot {
    pid_t pid = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;
    HeapBuf stdout_buf = {};
    HeapBuf stderr_buf = {};
    std::size_t job_index = 0;
    pup::SteadyClock::time_point start_time = {};

    auto active() const -> bool { return pid > 0; }

    auto reset() -> void
    {
        pid = -1;
        stdout_fd = -1;
        stderr_fd = -1;
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

    // Resolve all StringIds to string_view BEFORE fork.
    // After fork, pool storage is immutable (COW). Pool entries are
    // null-terminated (HeapBuf guarantee), safe for POSIX C APIs.
    auto command_str = pool.get(job.command);
    auto working_dir_str = pool.get(working_dir);

    auto env_strings = pup::platform::build_env_strings(env_ids, inherit_env);
    auto env_c_strs = Vec<std::string_view> {};
    env_c_strs.reserve(env_strings.size());
    for (auto s : env_strings) {
        env_c_strs.push_back(pool.get(s));
    }

    int stdout_pipe[2] = { -1, -1 };
    int stderr_pipe[2] = { -1, -1 };

    if (::pipe(stdout_pipe) < 0) {
        return false;
    }
    if (::pipe(stderr_pipe) < 0) {
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        return false;
    }

    auto pid = ::fork();
    if (pid < 0) {
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        return false;
    }

    if (pid == 0) {
        // Child process
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);

        ::dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);

        if (!working_dir_str.empty()) {
            if (::chdir(working_dir_str.data()) < 0) {
                ::_exit(127);
            }
        }

        auto env_ptrs = Vec<char*> {};
        env_ptrs.reserve(env_c_strs.size() + 1);
        for (auto s : env_c_strs) {
            env_ptrs.push_back(const_cast<char*>(s.data()));
        }
        env_ptrs.push_back(nullptr);

        // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast)
        char* const argv[] = {
            const_cast<char*>("/bin/sh"),
            const_cast<char*>("-c"),
            const_cast<char*>(command_str.data()),
            nullptr
        };
        // NOLINTEND(cppcoreguidelines-pro-type-const-cast)

        if (inherit_env && env_ids.empty()) {
            ::execv("/bin/sh", argv);
        } else {
            ::execve("/bin/sh", argv, env_ptrs.data());
        }

        ::_exit(127);
    }

    // Parent: close write ends, keep read ends
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);

    // Set read ends to non-blocking for poll()
    ::fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK); // NOLINT(cppcoreguidelines-pro-type-vararg)
    ::fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK); // NOLINT(cppcoreguidelines-pro-type-vararg)

    slot.pid = pid;
    slot.stdout_fd = stdout_pipe[0];
    slot.stderr_fd = stderr_pipe[0];
    slot.job_index = job_idx;
    slot.start_time = pup::SteadyClock::now();

    return true;
}

auto reap_slot(JobSlot& slot, int status) -> JobResult
{
    auto& pool = global_pool();
    auto result = JobResult {};
    result.id = 0;

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
        result.success = (result.exit_code == 0);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
        result.success = false;
    }

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

    if (slot.stdout_fd >= 0) {
        ::close(slot.stdout_fd);
    }
    if (slot.stderr_fd >= 0) {
        ::close(slot.stderr_fd);
    }

    slot.reset();
    return result;
}

auto kill_slot(JobSlot& slot) -> void
{
    if (slot.active()) {
        ::kill(slot.pid, SIGTERM);
    }
}

#endif // !_WIN32

} // namespace

struct Scheduler::Impl {
    SchedulerOptions options;
    BuildStats stats;
    bool cancelled = false;

    JobStartCallback on_start;
    JobCompleteCallback on_complete;
    ProgressCallback on_progress;

    auto execute_sequential(
        Vec<BuildJob> const& jobs,
        graph::BuildGraph const& graph,
        EnvCache const& env_cache
    ) -> Result<void>;

    auto execute_job(
        BuildJob const& job,
        CommandRunner& runner,
        EnvCache const& env_cache,
        std::string_view source_root_sv,
        std::string_view output_root_sv,
        std::string_view output_root_prefix
    ) -> JobResult;
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

auto Scheduler::build(graph::BuildGraph const& graph) -> Result<BuildStats>
{
    auto start_time = pup::SteadyClock::time_point { pup::SteadyClock::now() };
    impl_->cancelled = false;
    impl_->stats = BuildStats {};

    // Build job list in topological order
    auto jobs_result = build_job_list(graph);
    if (!jobs_result) {
        return pup::unexpected<Error>(jobs_result.error());
    }

    auto& jobs = *jobs_result;
    impl_->stats.total_jobs = jobs.size();

    if (jobs.empty()) {
        auto end_time = pup::SteadyClock::time_point { pup::SteadyClock::now() };
        impl_->stats.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time
        );
        return impl_->stats;
    }

    // Execute jobs
    auto exec_result = execute_parallel(jobs, graph);

    auto end_time = pup::SteadyClock::time_point { pup::SteadyClock::now() };
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
    Vec<StringId> const& changed_files
) -> Result<BuildStats>
{
    auto& pool = global_pool();

    // Build temporary path-to-NodeId map for changed file lookup
    // This is O(n) scan but only done once per incremental build
    auto path_to_id = Vec<std::pair<std::string_view, NodeId>> {};
    for (auto id : graph.all_nodes()) {
        auto path_id = graph.get_full_path(id);
        if (!is_empty(path_id)) {
            path_to_id.emplace_back(pool.get(path_id), id);
        }
    }
    std::sort(path_to_id.begin(), path_to_id.end());

    // Find all nodes affected by changes
    auto affected = NodeIdMap32 {};
    auto affected_vec = Vec<NodeId> {};
    for (auto file_id : changed_files) {
        auto file_path = pool.get(file_id);
        auto it = std::lower_bound(path_to_id.begin(), path_to_id.end(), file_path, [](auto const& p, auto const& k) { return p.first < k; });
        if (it != path_to_id.end() && it->first == file_path) {
            auto id = it->second;
            if (!affected.contains(id)) {
                affected.set(id, 1);
                affected_vec.push_back(id);
            }

            // For generated files that are missing/changed, also mark the producing command
            auto const* node = graph.get_file_node(id);
            if (node && node->type == NodeType::Generated) {
                for (auto input_id : graph.get_inputs(id)) {
                    if (!affected.contains(input_id)) {
                        affected.set(input_id, 1);
                        affected_vec.push_back(input_id);
                    }
                }
            }
        }
    }

    // Expand to include all dependent commands (including order-only)
    // get_outputs() excludes sticky edges by design (Tupfile/config dependencies
    // are parse-time deps, not build-time deps)
    auto to_process = Vec<NodeId> { affected_vec };

    while (!to_process.empty()) {
        auto id = NodeId { to_process.back() };
        to_process.pop_back();

        for (auto dep_id : graph.get_outputs(id)) {
            if (!affected.contains(dep_id)) {
                affected.set(dep_id, 1);
                to_process.push_back(dep_id);
            }
        }

        for (auto dep_id : graph.get_order_only_dependents(id)) {
            if (!affected.contains(dep_id)) {
                affected.set(dep_id, 1);
                to_process.push_back(dep_id);
            }
        }
    }

    // Build all jobs, then filter
    auto all_jobs = build_job_list(graph);
    if (!all_jobs) {
        return pup::unexpected<Error>(all_jobs.error());
    }

    auto jobs = filter_jobs(*all_jobs, affected);
    impl_->stats.total_jobs = jobs.size();
    impl_->stats.skipped_jobs = all_jobs->size() - jobs.size();

    if (jobs.empty()) {
        return impl_->stats;
    }

    auto exec_result = execute_parallel(jobs, graph);
    if (!exec_result && !impl_->options.keep_going) {
        return pup::unexpected<Error>(exec_result.error());
    }

    return impl_->stats;
}

auto Scheduler::Impl::execute_sequential(
    Vec<BuildJob> const& jobs,
    graph::BuildGraph const& graph,
    EnvCache const& env_cache
) -> Result<void>
{
    auto& pool = global_pool();
    auto runner = CommandRunner {};
    if (!is_empty(options.source_root)) {
        runner.set_working_dir(options.source_root);
    }
    if (options.timeout) {
        runner.set_timeout(*options.timeout); // NOLINT(bugprone-unchecked-optional-access)
    }

    auto source_root_sv = pool.get(options.source_root);
    auto output_root_sv = pool.get(options.output_root);
    auto output_root_prefix = pool.get(pup::path::relative(output_root_sv, source_root_sv));

    auto [in_degree, dependents] = build_dependency_map(jobs, graph);

    // Validate no active job depends on a skipped job
    if (auto result = validate_guard_dependencies(jobs, dependents); !result) {
        return pup::unexpected<Error>(result.error());
    }

    // Count inactive jobs upfront (they never enter the queue)
    auto inactive_count = std::count_if(jobs.begin(), jobs.end(), [](auto const& j) { return !j.guard_active; });
    stats.skipped_jobs += static_cast<std::size_t>(inactive_count);

    // Only queue active jobs with no dependencies
    auto ready_queue = std::queue<std::size_t> {};
    for (auto i = std::size_t { 0 }; i < jobs.size(); ++i) {
        if (in_degree[i] == 0 && jobs[i].guard_active) {
            ready_queue.push(i);
        }
    }

    while (!ready_queue.empty()) {
        if (cancelled) {
            break;
        }

        auto const job_idx = ready_queue.front();
        ready_queue.pop();
        auto const& job = jobs[job_idx];

        if (on_start) {
            on_start(job);
        }

        auto result = JobResult { execute_job(job, runner, env_cache, source_root_sv, output_root_sv, output_root_prefix) };

        if (on_complete) {
            on_complete(job, result);
        }

        stats.build_time += result.duration;

        if (result.success) {
            ++stats.completed_jobs;
            for (auto dep_idx : dependents[job_idx]) {
                if (--in_degree[dep_idx] == 0 && jobs[dep_idx].guard_active) {
                    ready_queue.push(dep_idx);
                }
            }
        } else {
            ++stats.failed_jobs;
            if (!options.keep_going) {
                return make_error<void>(ErrorCode::CommandFailed, "Command failed");
            }
        }

        if (on_progress) {
            on_progress(stats.completed_jobs + stats.failed_jobs, stats.total_jobs);
        }
    }

    return {};
}

auto Scheduler::execute_parallel(
    Vec<BuildJob> const& jobs,
    graph::BuildGraph const& graph
) -> Result<void>
{
    auto const env_cache = build_env_cache(jobs);

    if (impl_->options.jobs == 1 || jobs.size() == 1) {
        return impl_->execute_sequential(jobs, graph, env_cache);
    }

#ifdef _WIN32
    // Windows: fall back to sequential (no fork/poll)
    return impl_->execute_sequential(jobs, graph, env_cache);
#else

    auto& pool = global_pool();

    // Build dependency map
    auto [in_degree, dependents] = build_dependency_map(jobs, graph);

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

    // Pre-compute source/output roots for execute_job_pre/post
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

        // 2. Build pollfd array from active slots
        auto poll_fds = Vec<pollfd> {};
        poll_fds.reserve(running * 2);
        struct PollMap {
            std::size_t slot_idx;
            bool is_stderr;
        };
        auto poll_map = Vec<PollMap> {};
        poll_map.reserve(running * 2);

        for (auto i = std::size_t { 0 }; i < max_jobs; ++i) {
            if (!slots[i].active()) {
                continue;
            }
            if (slots[i].stdout_fd >= 0) {
                poll_fds.push_back(pollfd { .fd = slots[i].stdout_fd, .events = POLLIN, .revents = 0 });
                poll_map.push_back(PollMap { .slot_idx = i, .is_stderr = false });
            }
            if (slots[i].stderr_fd >= 0) {
                poll_fds.push_back(pollfd { .fd = slots[i].stderr_fd, .events = POLLIN, .revents = 0 });
                poll_map.push_back(PollMap { .slot_idx = i, .is_stderr = true });
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

        if (poll_fds.empty()) {
            poll_timeout = 100; // 100ms fallback for waitpid
        }

        // 3. Wait for I/O
        auto poll_result = ::poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()), poll_timeout);
        if (poll_result < 0 && errno == EINTR) {
            // Interrupted by signal -- check cancellation at top of next iteration
        }

        // 4. Drain ready pipes
        if (poll_result > 0) {
            char buf[4096];
            for (auto i = std::size_t { 0 }; i < poll_fds.size(); ++i) {
                if (!(poll_fds[i].revents & (POLLIN | POLLHUP))) {
                    continue;
                }
                auto n = ::read(poll_fds[i].fd, buf, sizeof(buf));
                if (n > 0) {
                    auto& slot = slots[poll_map[i].slot_idx];
                    auto data = std::string_view { buf, static_cast<std::size_t>(n) };
                    if (poll_map[i].is_stderr) {
                        slot.stderr_buf.append(data);
                    } else {
                        slot.stdout_buf.append(data);
                    }
                } else if (n == 0) {
                    // EOF on this FD -- close it so we don't poll it again
                    auto& slot = slots[poll_map[i].slot_idx];
                    if (poll_map[i].is_stderr) {
                        ::close(slot.stderr_fd);
                        slot.stderr_fd = -1;
                    } else {
                        ::close(slot.stdout_fd);
                        slot.stdout_fd = -1;
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
                    ::kill(slot.pid, SIGKILL);
                }
            }

            auto status = 0;
            auto wpid = ::waitpid(slot.pid, &status, WNOHANG);
            if (wpid <= 0) {
                continue; // Still running
            }

            // Drain any remaining pipe data before closing
            char buf[4096];
            if (slot.stdout_fd >= 0) {
                while (true) {
                    auto n = ::read(slot.stdout_fd, buf, sizeof(buf));
                    if (n > 0) {
                        slot.stdout_buf.append(std::string_view { buf, static_cast<std::size_t>(n) });
                    } else {
                        break;
                    }
                }
            }
            if (slot.stderr_fd >= 0) {
                while (true) {
                    auto n = ::read(slot.stderr_fd, buf, sizeof(buf));
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
                    auto status = 0;
                    auto wpid = ::waitpid(slots[si].pid, &status, WNOHANG);
                    if (wpid <= 0) {
                        ::kill(slots[si].pid, SIGKILL);
                        ::waitpid(slots[si].pid, &status, 0);
                    }
                    if (slots[si].stdout_fd >= 0) {
                        ::close(slots[si].stdout_fd);
                    }
                    if (slots[si].stderr_fd >= 0) {
                        ::close(slots[si].stderr_fd);
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

#endif // !_WIN32
}

auto Scheduler::Impl::execute_job(
    BuildJob const& job,
    CommandRunner& runner,
    EnvCache const& env_cache,
    std::string_view source_root_sv,
    std::string_view output_root_sv,
    std::string_view output_root_prefix
) -> JobResult
{
    auto& pool = global_pool();
    auto result = JobResult {
        .id = job.id,
        .success = false,
        .exit_code = 0,
        .output = {},
        .duration = {},
    };

    auto prepared = prepare_job_launch(job, env_cache, options, source_root_sv, output_root_sv, output_root_prefix);

    if (options.dry_run) {
        result.success = true;
        return result;
    }

    auto run_opts = RunOptions {};
    if (!is_empty(prepared.working_dir)) {
        run_opts.working_dir = prepared.working_dir;
    }
    run_opts.env = std::move(prepared.env_ids);

    auto cmd_result = runner.run(pool.get(job.command), run_opts);
    if (!cmd_result) {
        result.output = pool.intern("Failed to execute command");
        return result;
    }

    result.exit_code = cmd_result->exit_code;
    result.success = (cmd_result->exit_code == 0);
    result.duration = cmd_result->duration;

    auto stdout_sv = pool.get(cmd_result->stdout_output);
    auto stderr_sv = pool.get(cmd_result->stderr_output);
    auto output_buf = Buf {};
    if (!stdout_sv.empty()) {
        output_buf += stdout_sv;
    }
    if (!stderr_sv.empty()) {
        if (!output_buf.empty()) {
            output_buf += '\n';
        }
        output_buf += stderr_sv;
    }
    if (!output_buf.empty()) {
        result.output = output_buf.intern(pool);
    }

    if (result.success) {
        parse_stdout_depfile(job, stdout_sv, result.discovered_deps, result.deps_for_command);
        discover_d_file_deps(job, result, source_root_sv, output_root_sv, output_root_prefix);
    }

    return result;
}

auto Scheduler::build_job_list(
    graph::BuildGraph const& graph
) -> Result<Vec<BuildJob>>
{
    auto job_list_start = pup::SteadyClock::now();

    // Get topological order
    auto topo_result = graph::TopoSortResult { graph::topological_sort(graph) };
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
    for (auto id : topo_result.order) {
        auto const* node = graph.get_file_node(id);
        if (node && node->type == NodeType::Ghost && !graph.get_outputs(id).empty()) {
            auto path_id = graph.get_full_path(id);
            auto path = pool.get(path_id);
            auto build_root_name = graph.get_build_root_name();
            auto file_path_sv = pool.get(pup::path::join(output_root_sv, path));
            auto lookup_path = path;
            auto build_prefix = Buf {};
            build_prefix += build_root_name;
            build_prefix += '/';
            if (!build_root_name.empty() && path.starts_with(build_prefix.view())) {
                lookup_path = path.substr(build_prefix.size());
                file_path_sv = pool.get(pup::path::join(output_root_sv, lookup_path));
            }
            if (pup::platform::exists(file_path_sv)) {
                continue;
            }
            auto err = Buf {};
            err.fmt("Missing input file (unresolved ghost): {}\n  Hint: try building with -a to include upstream dependencies", path);
            return make_error<Vec<BuildJob>>(ErrorCode::ParseError, err.view());
        }
    }

    auto jobs = Vec<BuildJob> {};
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
        auto working_dir_id = StringId::Empty;
        if (!source_dir.empty()) {
            working_dir_id = pup::path::join(source_root_sv, source_dir);
        } else {
            working_dir_id = pool.intern(source_root_sv);
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
        auto cmd_id = expand_instruction(graph.graph(), id, cache, source_root_sv, config_root_sv);
        auto display_id = node->display;

        auto exported_ids = Vec<StringId> {};
        exported_ids.reserve(node->exported_vars.size());
        for (auto raw_id : node->exported_vars) {
            exported_ids.push_back(make_string_id(raw_id));
        }

        // Evaluate guards - command only executes if ALL guards are satisfied
        auto guard_active = graph::is_guard_satisfied(graph.graph(), *node);

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
        for (auto input_id : graph.get_inputs(id)) {
            auto input_path = graph.get_full_path(input_id);
            if (!is_empty(input_path)) {
                job.inputs.push_back(input_path);
            }
        }

        // Collect output paths
        for (auto output_id : graph.get_outputs(id)) {
            auto output_path = graph.get_full_path(output_id);
            if (!is_empty(output_path)) {
                job.outputs.push_back(output_path);
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
                for (auto member_id : graph.get_inputs(oi_id)) {
                    auto member_path = graph.get_full_path(member_id);
                    if (!is_empty(member_path)) {
                        job.order_only_inputs.push_back(member_path);
                    }
                }
            } else {
                auto oi_path = graph.get_full_path(oi_id);
                if (!is_empty(oi_path)) {
                    job.order_only_inputs.push_back(oi_path);
                }
            }
        }

        jobs.push_back(std::move(job));
    }

    auto job_list_elapsed = pup::SteadyClock::now() - job_list_start;
    thread_metrics().job_list_time += std::chrono::duration_cast<std::chrono::microseconds>(job_list_elapsed);

    return jobs;
}

auto Scheduler::build_subset(
    graph::BuildGraph const& graph,
    NodeIdMap32 const& command_ids
) -> Result<BuildStats>
{
    auto start_time = pup::SteadyClock::time_point { pup::SteadyClock::now() };
    impl_->cancelled = false;
    impl_->stats = BuildStats {};

    // Build all jobs, then filter to the subset
    auto all_jobs = build_job_list(graph);
    if (!all_jobs) {
        return pup::unexpected<Error>(all_jobs.error());
    }

    auto jobs = filter_jobs(*all_jobs, command_ids);
    impl_->stats.total_jobs = jobs.size();
    impl_->stats.skipped_jobs = all_jobs->size() - jobs.size();

    if (jobs.empty()) {
        auto end_time = pup::SteadyClock::time_point { pup::SteadyClock::now() };
        impl_->stats.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time
        );
        return impl_->stats;
    }

    auto exec_result = execute_parallel(jobs, graph);

    auto end_time = pup::SteadyClock::time_point { pup::SteadyClock::now() };
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
    Vec<NodeId> const& target_ids
) -> Result<BuildStats>
{
    auto start_time = pup::SteadyClock::time_point { pup::SteadyClock::now() };
    impl_->cancelled = false;
    impl_->stats = BuildStats {};

    // Collect all commands needed to build these targets via reverse traversal
    auto required_cmds = collect_required_commands(graph, target_ids);

    // Build all jobs, then filter to required commands
    auto all_jobs = build_job_list(graph);
    if (!all_jobs) {
        return pup::unexpected<Error>(all_jobs.error());
    }

    auto jobs = filter_jobs(*all_jobs, required_cmds);
    impl_->stats.total_jobs = jobs.size();
    impl_->stats.skipped_jobs = all_jobs->size() - jobs.size();

    if (jobs.empty()) {
        auto end_time = pup::SteadyClock::time_point { pup::SteadyClock::now() };
        impl_->stats.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time
        );
        return impl_->stats;
    }

    auto exec_result = execute_parallel(jobs, graph);

    auto end_time = pup::SteadyClock::time_point { pup::SteadyClock::now() };
    impl_->stats.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time
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

auto detect_parallelism() -> std::size_t
{
    return pup::cpu_count();
}

} // namespace pup::exec
