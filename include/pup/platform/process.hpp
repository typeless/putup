// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/result.hpp"
#include "pup/core/string.hpp"
#include "pup/core/vec.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace pup::platform {

/// Result of process execution
struct ProcessResult {
    int exit_code = 0;
    String stdout_output;
    String stderr_output;
    std::chrono::milliseconds duration = {};
    bool timed_out = false;
    bool signaled = false;
    int signal = 0;
};

/// Options for process execution
struct ProcessOptions {
    String command;
    String working_dir;
    Vec<std::string> env;
    bool inherit_env = true;
    bool capture_stdout = true;
    bool capture_stderr = true;
    std::optional<std::string> stdin_data;
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
    Vec<std::string> const& extra_env,
    bool inherit_env
) -> Vec<std::string>;

} // namespace pup::platform
