// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <chrono>
#include <optional>

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

} // namespace pup::platform
