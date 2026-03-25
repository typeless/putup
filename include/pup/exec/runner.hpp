// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/result.hpp"
#include "pup/core/string.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace pup::exec {

/// Result of running a command
struct CommandResult {
    int exit_code = 0;
    String stdout_output = {};
    String stderr_output = {};
    std::chrono::milliseconds duration = {};
    bool timed_out = false;
    bool signaled = false;
    int signal = 0;
};

/// Options for running a command
struct RunOptions {
    String working_dir = {};
    Vec<std::string> env = {};                ///< Additional environment variables (KEY=VALUE for setenv)
    bool inherit_env = true;                          ///< Inherit parent environment
    std::optional<std::chrono::seconds> timeout = {}; ///< Command timeout
    bool capture_stdout = true;
    bool capture_stderr = true;
    std::optional<std::string> stdin_data = {}; ///< Data to pipe to stdin
};

/// Callback for command output
using OutputCallback = std::function<void(std::string_view, bool is_stderr)>;

/// Command runner - executes shell commands
class CommandRunner {
public:
    CommandRunner() = default;
    explicit CommandRunner(RunOptions default_options);

    /// Run a command and wait for completion
    [[nodiscard]]
    auto run(std::string_view command) -> Result<CommandResult>;

    /// Run a command with specific options
    [[nodiscard]]
    auto run(
        std::string_view command,
        RunOptions const& options
    ) -> Result<CommandResult>;

    /// Run a command with real-time output callback
    [[nodiscard]]
    auto run_with_output(
        std::string_view command,
        OutputCallback const& callback,
        RunOptions const& options = {}
    ) -> Result<CommandResult>;

    /// Set the default working directory
    auto set_working_dir(String dir) -> void
    {
        default_options_.working_dir = std::move(dir);
    }

    /// Add an environment variable to defaults
    auto add_env(std::string var) -> void
    {
        default_options_.env.push_back(std::move(var));
    }

    /// Set default timeout
    auto set_timeout(std::chrono::seconds timeout) -> void
    {
        default_options_.timeout = timeout;
    }

private:
    RunOptions default_options_ = {};

    [[nodiscard]]
    auto merge_options(RunOptions const& options) const -> RunOptions;
};

/// Parse a command string into shell arguments
[[nodiscard]]
auto parse_command(std::string_view command) -> Vec<std::string>;

/// Quote a string for shell use
[[nodiscard]]
auto shell_quote(std::string_view str) -> std::string;

} // namespace pup::exec
