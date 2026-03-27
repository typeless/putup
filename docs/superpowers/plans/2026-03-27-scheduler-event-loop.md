# Scheduler Event Loop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the thread-pool parallel scheduler with a single-threaded poll()-based event loop to eliminate fork+mutex and inherited-pipe race conditions.

**Architecture:** The new `execute_parallel` uses a fixed-size array of job slots (one per concurrent child). A single-threaded loop launches children into empty slots, polls their pipes for output, reaps finished children, and enqueues dependents. No threads, no mutexes, no condition variables.

**Tech Stack:** POSIX `pipe()`, `fork()`, `poll()`, `waitpid()`, `fcntl()`, `kill()`. Existing `HeapBuf` for output accumulation. Existing `build_dependency_map` and `EnvCache` unchanged.

---

### Task 1: Write failing test for keep-going parallel execution

The existing `[keep-going]` test hangs ~70-90% of the time due to the threading bugs. We'll first verify it fails reliably, establishing our red baseline.

**Files:**
- Test: `test/unit/test_e2e.cpp` (existing test, no changes needed)

- [ ] **Step 1: Run the keep-going test to confirm it hangs**

```bash
timeout 15 ./build/test/unit/putup_test "Scenario: Partial failure with -k saves successful outputs" 2>&1 | tail -5
```

Expected: Either SIGTERM (timeout killed it) or test failure. This confirms the bug exists.

- [ ] **Step 2: Run the full E2E suite to establish baseline**

```bash
timeout 60 ./build/test/unit/putup_test '[e2e]' 2>&1 | tail -3
```

Expected: 1 failure (the keep-going hang). Record total test count for later comparison.

---

### Task 2: Add JobSlot struct and launch_job helper

This task adds the data structure for in-flight jobs and the fork/exec helper that populates a slot. No behavior change yet -- just new code.

**Files:**
- Modify: `src/exec/scheduler.cpp` (add to anonymous namespace)

- [ ] **Step 1: Add POSIX includes and JobSlot struct**

Add these includes at the top of `src/exec/scheduler.cpp`, after the existing includes (around line 22):

```cpp
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
```

Add the `JobSlot` struct inside the anonymous namespace (after the existing `EnvCache` definitions, around line 115):

```cpp
#ifndef _WIN32

struct JobSlot {
    pid_t pid = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;
    HeapBuf stdout_buf = {};
    HeapBuf stderr_buf = {};
    std::size_t job_index = 0;
    std::chrono::steady_clock::time_point start_time = {};

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

#endif // !_WIN32
```

- [ ] **Step 2: Add launch_job helper**

Add after the `JobSlot` struct, still inside `#ifndef _WIN32` and the anonymous namespace:

```cpp
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

    // Resolve all StringIds to owned strings BEFORE fork.
    // After fork, we must not touch the StringPool (single-thread safety).
    auto command_str = String { pool.get(job.command) };
    auto working_dir_str = String { pool.get(working_dir) };

    auto env_strings = pup::platform::build_env_strings(env_ids, inherit_env);
    auto env_c_strs = Vec<String> {};
    env_c_strs.reserve(env_strings.size());
    for (auto s : env_strings) {
        env_c_strs.push_back(String { pool.get(s) });
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
        for (auto& s : env_c_strs) {
            env_ptrs.push_back(s.data());
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
    slot.start_time = std::chrono::steady_clock::now();

    return true;
}
```

- [ ] **Step 3: Add reap_slot helper**

Add after `launch_job`, still inside `#ifndef _WIN32`:

```cpp
auto reap_slot(JobSlot& slot, int status) -> JobResult
{
    auto& pool = global_pool();
    auto result = JobResult {};
    result.id = 0; // caller sets this from the BuildJob

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
        result.success = (result.exit_code == 0);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
        result.success = false;
    }

    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - slot.start_time
    );

    // Build combined output (same logic as execute_job)
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

    // Close any remaining FDs
    if (slot.stdout_fd >= 0) {
        ::close(slot.stdout_fd);
    }
    if (slot.stderr_fd >= 0) {
        ::close(slot.stderr_fd);
    }

    slot.reset();
    return result;
}

#endif // !_WIN32
```

- [ ] **Step 4: Add kill_slot helper**

Add before the `#endif // !_WIN32` that closes the block:

```cpp
auto kill_slot(JobSlot& slot) -> void
{
    if (slot.active()) {
        ::kill(slot.pid, SIGTERM);
    }
}
```

- [ ] **Step 5: Verify it compiles**

```bash
make 2>&1 | tail -5
```

Expected: Build succeeds. No behavior change yet -- these are unused helpers.

- [ ] **Step 6: Commit**

```bash
git add src/exec/scheduler.cpp
git commit -m "Add JobSlot, launch_job, reap_slot helpers for event loop scheduler"
```

---

### Task 3: Rewrite execute_parallel as event loop

Replace the thread-pool implementation with the single-threaded poll()-based event loop. This is the core change.

**Files:**
- Modify: `src/exec/scheduler.cpp:524-684` (replace `execute_parallel` body)

- [ ] **Step 1: Replace execute_parallel with event loop**

Replace the entire body of `Scheduler::execute_parallel` (lines 524-684) with:

```cpp
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

    // Job slots -- one per concurrent child process
    auto max_jobs = std::min(impl_->options.jobs, active_count);
    auto slots = Vec<JobSlot> {};
    slots.resize(max_jobs);

    auto running = std::size_t { 0 };
    auto finished_count = std::size_t { 0 };
    auto failed = false;

    // Pre-compute source/output roots for execute_job_pre/post
    auto source_root_sv = pool.get(impl_->options.source_root);
    auto output_root_sv = pool.get(impl_->options.output_root);
    auto relative_output_root = pup::path::relative(output_root_sv, source_root_sv);
    auto output_root_prefix = relative_output_root;

    while (finished_count < active_count) {
        // 1. Launch: fill empty slots from ready_queue
        if (!failed || impl_->options.keep_going) {
            for (auto& slot : slots) {
                if (slot.active() || ready_queue.empty()) {
                    continue;
                }

                auto job_idx = ready_queue.front();
                auto const& job = jobs[job_idx];

                // Pre-launch: ensure output directories exist (same as execute_job)
                for (auto output_id : job.outputs) {
                    auto output_sv = pool.get(output_id);
                    auto output_path = pup::path::is_absolute(output_sv)
                        ? String { output_sv }
                        : resolve_variant_path(source_root_sv, output_root_sv, output_root_prefix, output_sv);
                    auto parent = pup::path::parent(output_path);
                    if (!parent.empty()) {
                        (void)pup::platform::create_directories(parent);
                    }
                }

                // Build env for this job
                auto env_ids = Vec<StringId> {};
                for (auto var_id : job.exported_vars) {
                    auto var_sv = pool.get(var_id);
                    if (auto it = env_cache_find(env_cache, var_sv); it != env_cache.end()) {
                        auto entry = String { var_sv };
                        entry += '=';
                        entry += it->second;
                        env_ids.push_back(pool.intern(entry));
                    }
                }

                auto working_dir = job.working_dir;
                if (is_empty(working_dir)) {
                    working_dir = impl_->options.source_root;
                }

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

                if (!launch_job(slot, job, job_idx, working_dir, env_ids, true)) {
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
        if (running == 0) {
            break;
        }

        // 2. Build pollfd array from active slots
        auto poll_fds = Vec<pollfd> {};
        poll_fds.reserve(running * 2);
        // Map from pollfd index back to slot index and fd type
        struct PollMap {
            std::size_t slot_idx;
            bool is_stderr;
        };
        auto poll_map = Vec<PollMap> {};
        poll_map.reserve(running * 2);

        for (auto i = std::size_t { 0 }; i < slots.size(); ++i) {
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
            auto now = std::chrono::steady_clock::now();
            auto min_remaining = std::chrono::milliseconds::max();
            for (auto const& slot : slots) {
                if (!slot.active()) {
                    continue;
                }
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - slot.start_time);
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*impl_->options.timeout) - elapsed;
                if (remaining < min_remaining) {
                    min_remaining = remaining;
                }
            }
            poll_timeout = std::max(static_cast<int>(min_remaining.count()), 0);
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
        for (auto& slot : slots) {
            if (!slot.active()) {
                continue;
            }

            // Check for timeout
            if (impl_->options.timeout) {
                auto elapsed = std::chrono::steady_clock::now() - slot.start_time;
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

            // Save stdout before reap clears the buffer (needed for depfile parsing)
            auto stdout_copy = String { slot.stdout_buf.view() };

            auto result = reap_slot(slot, status);
            result.id = job.id;
            --running;

            // Discover implicit deps (same logic as execute_job)
            if (result.success && job.inject_implicit_deps) {
                // Parse stdout as depfile (inject_implicit_deps captures
                // compiler output via -MD/-MF). We saved stdout_view before
                // reap_slot cleared the buffer.
                auto depfile_result = parser::parse_depfile(stdout_copy);
                if (depfile_result) {
                    for (auto dep_id : depfile_result->dependencies) {
                        result.discovered_deps.push_back(dep_id);
                    }
                    result.deps_for_command = job.parent_command;
                }
            }

            if (result.success) {
                // Discover implicit deps from .d files
                auto ext_check = [&](StringId output_id) {
                    auto output_sv = pool.get(output_id);
                    auto ext = pup::path::extension(output_sv);
                    if (ext != ".o" && ext != ".obj") {
                        return;
                    }
                    auto base_path = resolve_variant_path(source_root_sv, output_root_sv, output_root_prefix, pup::path::parent(output_sv));
                    auto stem = String { pup::path::stem(output_sv) };
                    stem += ".d";
                    auto depfile_path = pup::path::join(base_path, stem);
                    if (!pup::platform::exists(depfile_path)) {
                        return;
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
                };

                for (auto output_id : job.outputs) {
                    ext_check(output_id);
                }
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
        if (impl_->cancelled.load() || (failed && !impl_->options.keep_going)) {
            // Kill remaining children
            for (auto& slot : slots) {
                kill_slot(slot);
            }
            // Reap all
            for (auto& slot : slots) {
                if (slot.active()) {
                    auto status = 0;
                    ::waitpid(slot.pid, &status, 0); // blocking
                    if (slot.stdout_fd >= 0) {
                        ::close(slot.stdout_fd);
                    }
                    if (slot.stderr_fd >= 0) {
                        ::close(slot.stderr_fd);
                    }
                    slot.reset();
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
```

- [ ] **Step 2: Remove unused threading includes**

Remove these includes from the top of `src/exec/scheduler.cpp` (they are no longer used by `execute_parallel`):

```cpp
// Remove these lines:
#include <condition_variable>
#include <thread>
```

Keep `<mutex>` only if used elsewhere in the file. Check with:
```bash
grep -n "mutex\|condition_variable\|std::thread" src/exec/scheduler.cpp
```

If nothing else uses them, remove them.

- [ ] **Step 3: Verify it compiles**

```bash
make 2>&1 | tail -5
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/exec/scheduler.cpp
git commit -m "Replace thread-pool scheduler with single-threaded poll() event loop

Eliminates fork+mutex deadlock and inherited-pipe-FD race conditions
by removing all threading from the parallel executor. Parallelism now
comes from running multiple child processes concurrently, multiplexed
via poll(). This matches how Make, Ninja, and Tup handle parallel builds."
```

---

### Task 4: Verify the keep-going test passes

The primary regression test that was hanging should now work reliably.

**Files:**
- No changes -- just testing

- [ ] **Step 1: Run the keep-going test 10 times**

```bash
for i in $(seq 1 10); do
    result=$(timeout 15 ./build/test/unit/putup_test "Scenario: Partial failure with -k saves successful outputs" 2>&1 | grep "test cases")
    echo "Run $i: $result"
done
```

Expected: All 10 runs show `All tests passed (11 assertions in 1 test case)`. Zero timeouts.

- [ ] **Step 2: Run the full E2E suite**

```bash
timeout 120 ./build/test/unit/putup_test '[e2e]' 2>&1 | tail -5
```

Expected: All tests pass (same count as Task 1, but 0 failures).

- [ ] **Step 3: Run the full test suite**

```bash
timeout 120 ./build/test/unit/putup_test 2>&1 | tail -5
```

Expected: All tests pass.

---

### Task 5: Verify self-host build

The scheduler change must not break putup's ability to build itself.

**Files:**
- No changes -- just testing

- [ ] **Step 1: Clean build with the new scheduler**

```bash
make clean && make 2>&1 | tail -10
```

Expected: Build succeeds.

- [ ] **Step 2: Verify the built binary works**

```bash
./build/putup parse -v 2>&1 | tail -3
```

Expected: Parse succeeds.

- [ ] **Step 3: Run format and tidy checks**

```bash
make format 2>&1 | tail -3
make tidy 2>&1 | tail -3
```

Expected: No formatting issues. No tidy warnings. If clang-format produces changes, apply them and amend the commit from Task 3.

- [ ] **Step 4: Commit formatting fixes (if any)**

```bash
git add -u
git commit -m "Apply clang-format to event loop scheduler"
```

---

### Task 6: Push and verify CI

**Files:**
- No changes

- [ ] **Step 1: Push to main**

```bash
git push
```

- [ ] **Step 2: Monitor CI**

```bash
gh run list -L 1
gh run watch <run-id> --exit-status
```

Expected: All jobs pass -- Linux, macOS, Windows (Windows falls back to sequential), tidy, format-check.

- [ ] **Step 3: Verify the keep-going test specifically passed in CI**

Check the test-linux job logs to confirm the keep-going test ran and passed (it was previously hanging CI).
