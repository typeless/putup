// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>

namespace pup::platform {

/// Result of process execution
struct ProcessResult {
    int exit_code = 0;
    StringId stdout_output = StringId::Empty;
    StringId stderr_output = StringId::Empty;
    std::chrono::milliseconds duration = {};
    bool timed_out = false;
    bool signaled = false;
    int signal = 0;
};

/// Options for process execution
struct ProcessOptions {
    StringId command = StringId::Empty;
    StringId working_dir = StringId::Empty;
    Vec<StringId> env;
    bool inherit_env = true;
    bool capture_stdout = true;
    bool capture_stderr = true;
    std::optional<StringId> stdin_data;
    std::optional<std::chrono::seconds> timeout;
};

/// Callback for streaming process output
using ProcessOutputCallback = void (*)(std::string_view data, bool is_stderr, void* user_data);

/// Run a process and wait for completion
[[nodiscard]]
auto run_process(ProcessOptions const& opts) -> Result<ProcessResult>;

/// Run a process with streaming output callback
[[nodiscard]]
auto run_process_with_callback(
    ProcessOptions const& opts,
    ProcessOutputCallback callback,
    void* user_data
) -> Result<ProcessResult>;

/// Build environment variable list from options
[[nodiscard]]
auto build_env_strings(
    Vec<StringId> const& extra_env,
    bool inherit_env
) -> Vec<StringId>;

/// Run tasks in parallel child processes (POSIX) or sequentially (Windows).
/// Each task is a function returning an exit code (0 = success).
/// Returns the number of tasks that returned non-zero.
[[nodiscard]]
auto run_parallel_tasks(
    int (*task)(void* ctx),
    void** contexts,
    std::size_t count
) -> int;

struct AsyncProcess final {
    int pid = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;

    [[nodiscard]]
    auto active() const -> bool { return pid > 0; }
};

struct SpawnOptions final {
    std::string_view command;
    std::string_view working_dir;
    char* const* env = nullptr; // nullptr = inherit
};

struct ProcessStatus final {
    bool exited = false;
    int exit_code = 0;
};

struct PollableFd final {
    int fd;
    bool is_stderr;
    std::size_t slot_index;
};

enum class Signal { Terminate, Kill };

[[nodiscard]]
auto spawn_async(SpawnOptions const& opts) -> Result<AsyncProcess>;

[[nodiscard]]
auto poll_fds(PollableFd* fds, std::size_t count, int timeout_ms) -> int;

[[nodiscard]]
auto read_nonblocking(int fd, char* buf, std::size_t size) -> int;

auto close_fd(int fd) -> void;

[[nodiscard]]
auto try_reap(int pid, ProcessStatus& out) -> bool;

auto reap(int pid, ProcessStatus& out) -> void;

auto send_signal(int pid, Signal sig) -> void;

} // namespace pup::platform
