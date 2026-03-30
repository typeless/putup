# Scheduler Event Loop Redesign

## Problem

The parallel scheduler creates N worker threads, each of which independently forks child processes. This causes two classes of POSIX bugs:

1. **Inherited pipe file descriptors**: When Thread A creates pipes and forks, Thread B's child inherits Thread A's pipe write-ends. Without `O_CLOEXEC`, these survive the exec call, preventing Thread A's `poll()` from seeing EOF until Thread B's child exits. This creates phantom cross-dependencies between unrelated jobs.

2. **Inherited mutex state**: After `fork()`, only the calling thread survives. Mutexes held by other threads are permanently locked in the child. The child process calls `global_pool().intern()` and `global_pool().get()` (which lock the StringPool mutex) between `fork()` and the exec call. If another worker held the mutex at fork time, the child deadlocks.

These are not edge cases -- they are fundamental to how `fork()` works in multi-threaded programs. The result is a flaky hang in the `keep-going` E2E test (~70-90% failure rate) and potentially any parallel build with concurrent job launches.

## Approach

Replace the thread-pool model in `execute_parallel` with a single-threaded event loop using `poll()` for I/O multiplexing. This matches how Make, Ninja, and Tup handle parallel execution.

Parallelism comes from running multiple child processes concurrently, not from multiple threads. One thread does all forking, pipe reading, and reaping.

## Design

### Event Loop Structure

```
prepare ready_queue from in_degree=0 jobs
resolve env_cache

while (completed + failed < active_count) {
    1. Launch: fill empty job slots from ready_queue
       while (running < max_jobs && !ready_queue.empty())
           job = ready_queue.pop()
           slot = launch_job(job)
           on_start(job)

    2. Wait: poll() on all active slots' pipes
       poll(all_pipe_fds, timeout)

    3. Drain: read available data from ready pipes
       for each slot with POLLIN:
           read chunk into slot buffer

    4. Reap: check for finished children
       for each slot:
           if waitpid(pid, WNOHANG) returned:
               close pipes, build JobResult
               on_complete(job, result)
               update stats
               enqueue dependents whose in_degree hits 0
               free slot

    5. Check termination conditions
       if cancelled or (failed and not keep_going):
           kill remaining children, break
}

reap any remaining children
```

### Job Slot

A fixed-size array of `max_jobs` slots. Each slot represents one in-flight child process:

```cpp
struct JobSlot {
    pid_t pid = -1;                                // -1 = empty
    int stdout_fd = -1;
    int stderr_fd = -1;
    HeapBuf stdout_buf;
    HeapBuf stderr_buf;
    std::size_t job_index = 0;
    std::chrono::steady_clock::time_point start_time;
};
```

Launching a job claims an empty slot (`pid == -1`). Reaping a child frees the slot.

The `pollfd` array is rebuilt each iteration from active slots -- at most `2 * max_jobs` entries (stdout + stderr per slot). With typical `-j8`, that's 16 FDs.

### Helpers

```cpp
// Creates pipes, forks, runs the command. Populates slot on success.
auto launch_job(JobSlot& slot, BuildJob const& job, EnvCache const&) -> bool;

// Closes pipes, builds JobResult from slot buffers. Resets slot to empty.
auto reap_slot(JobSlot& slot, BuildJob const& job) -> JobResult;
```

`launch_job` contains the fork+pipe logic that currently lives in `run_process_with_callback`. It creates pipes, resolves StringIds to owned strings, forks, sets up redirections in the child, and runs `/bin/sh -c <command>`. The parent records the pid and pipe read-ends in the slot.

`reap_slot` is called after `waitpid` confirms the child exited. It closes remaining pipe FDs, extracts exit code/signal info, and returns a `JobResult` built from the slot's accumulated buffers.

### Pipe I/O

Each poll iteration reads one `read()` call worth of data (up to 4KB) per ready FD. This ensures fairness -- no single chatty child starves the loop. Output accumulates in the slot's `HeapBuf` across iterations.

No cap on buffer size. If a command produces excessive output, that is the user's concern. The `capture_stdout` / `capture_stderr` flags on the job control whether output is captured at all.

### Error Handling

**Job failure (`keep_going=false`)**: Stop launching new jobs. Send `SIGTERM` to all running children, drain pipes, reap. Return error.

**Job failure (`keep_going=true`)**: Record failure, skip dependents of the failed job, keep launching other ready jobs. Return with `failed_jobs > 0` after all runnable jobs complete.

**Cancellation**: Same as failure without keep_going. The `cancelled` flag is checked once per loop iteration.

**Fork failure**: Treat as job failure (`JobResult.success = false`).

**Timeout**: Each slot records `start_time`. Before `poll()`, compute the shortest remaining deadline across active slots. Pass as poll timeout. Kill children that exceed their deadline.

**EINTR**: `poll()` returns -1 with `errno == EINTR` on signal delivery. Retry the loop -- the cancellation check handles SIGINT.

## Scope

### Changes

- `Scheduler::execute_parallel` in `src/exec/scheduler.cpp` -- rewritten from thread pool to event loop
- New `JobSlot` struct and `launch_job` / `reap_slot` helpers in the same file

### Unchanged

- `Scheduler` public API -- no signature changes
- `execute_sequential` -- already single-threaded
- `run_process_with_callback` in `process-posix.cpp` -- still used by `execute_sequential`, E2E fixture, CommandRunner
- `BuildJob`, `JobResult`, `BuildStats`, callback types
- `build_dependency_map`, `validate_guard_dependencies`
- `cmd_build.cpp`, `multi_variant.cpp` -- callers untouched
- `process-win32.cpp` -- unaffected

### Deleted

- Worker thread lambda, `Vec<std::thread>`, `std::mutex`, `std::condition_variable`, `std::atomic<bool>` flags in `execute_parallel`

### Net Effect

Threading and synchronization code (~80 lines) replaced by event loop (~100-120 lines). No new files. No new dependencies. The fork+mutex and inherited-pipe bugs are eliminated structurally:

- **No threads during fork** -- a single thread does all forking, so no mutex can be held by "another thread" at fork time.
- **No pipe write-end leaks** -- the parent closes each child's pipe write-ends immediately after fork. Other slots' write-ends were already closed by the parent before. A new child only inherits read-ends from other slots (held by the parent for poll), which don't prevent EOF on those slots.

## Testing

The existing test suite covers this:

- `[keep-going]` tag -- the flaky test that currently hangs becomes the primary regression test
- `[exec]` tag -- scheduler unit tests (parallel execution, dependencies, cancellation)
- `[e2e]` tag -- full integration tests that exercise parallel builds
- Self-host build -- `putup -B build` builds itself using the scheduler

No new tests needed -- the behavior is identical, only the implementation changes.
