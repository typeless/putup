// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "pup/core/result.hpp"
#include "pup/core/types.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pup::exec {

/// Result of running a command
struct CommandResult {
    int exit_code = 0;
    std::string stdout_output = {};
    std::string stderr_output = {};
    std::chrono::milliseconds duration = {};
    bool timed_out = false;
    bool signaled = false;
    int signal = 0;
};

/// Options for running a command
struct RunOptions {
    std::filesystem::path working_dir = {};
    std::vector<std::string> env = {};                ///< Additional environment variables
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
    [[nodiscard]] auto run(std::string_view command) -> Result<CommandResult>;

    /// Run a command with specific options
    [[nodiscard]] auto run(
        std::string_view command,
        RunOptions const& options
    ) -> Result<CommandResult>;

    /// Run a command with real-time output callback
    [[nodiscard]] auto run_with_output(
        std::string_view command,
        OutputCallback const& callback,
        RunOptions const& options = {}
    ) -> Result<CommandResult>;

    /// Set the default working directory
    auto set_working_dir(std::filesystem::path dir) -> void
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

    [[nodiscard]] auto merge_options(RunOptions const& options) const -> RunOptions;
    [[nodiscard]] auto build_env(RunOptions const& options) const -> std::vector<std::string>;
};

/// Parse a command string into shell arguments
[[nodiscard]] auto parse_command(std::string_view command) -> std::vector<std::string>;

/// Quote a string for shell use
[[nodiscard]] auto shell_quote(std::string_view str) -> std::string;

} // namespace pup::exec
