// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/exec/runner.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace pup::exec {

CommandRunner::CommandRunner(RunOptions default_options)
    : default_options_(std::move(default_options))
{
}

auto CommandRunner::run(std::string_view command) -> Result<CommandResult>
{
    return run(command, default_options_);
}

auto CommandRunner::run(std::string_view command, RunOptions const& options)
    -> Result<CommandResult>
{
    return run_with_output(command, nullptr, options);
}

auto CommandRunner::run_with_output(
    std::string_view command,
    OutputCallback const& callback,
    RunOptions const& options) -> Result<CommandResult>
{
    auto merged = merge_options(options);
    auto start_time = std::chrono::steady_clock::now();

    // Create pipes for stdout and stderr
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    int stdin_pipe[2] = {-1, -1};

    if (merged.capture_stdout && ::pipe(stdout_pipe) < 0)
        return make_error<CommandResult>(ErrorCode::IoError, "Failed to create stdout pipe");

    if (merged.capture_stderr && ::pipe(stderr_pipe) < 0) {
        if (stdout_pipe[0] >= 0) {
            ::close(stdout_pipe[0]);
            ::close(stdout_pipe[1]);
        }
        return make_error<CommandResult>(ErrorCode::IoError, "Failed to create stderr pipe");
    }

    if (merged.stdin_data && ::pipe(stdin_pipe) < 0) {
        if (stdout_pipe[0] >= 0) {
            ::close(stdout_pipe[0]);
            ::close(stdout_pipe[1]);
        }
        if (stderr_pipe[0] >= 0) {
            ::close(stderr_pipe[0]);
            ::close(stderr_pipe[1]);
        }
        return make_error<CommandResult>(ErrorCode::IoError, "Failed to create stdin pipe");
    }

    auto pid = ::fork();
    if (pid < 0) {
        // Fork failed
        if (stdout_pipe[0] >= 0) {
            ::close(stdout_pipe[0]);
            ::close(stdout_pipe[1]);
        }
        if (stderr_pipe[0] >= 0) {
            ::close(stderr_pipe[0]);
            ::close(stderr_pipe[1]);
        }
        if (stdin_pipe[0] >= 0) {
            ::close(stdin_pipe[0]);
            ::close(stdin_pipe[1]);
        }
        return make_error<CommandResult>(ErrorCode::IoError, "Failed to fork");
    }

    if (pid == 0) {
        // Child process
        if (stdout_pipe[1] >= 0) {
            ::dup2(stdout_pipe[1], STDOUT_FILENO);
            ::close(stdout_pipe[0]);
            ::close(stdout_pipe[1]);
        }

        if (stderr_pipe[1] >= 0) {
            ::dup2(stderr_pipe[1], STDERR_FILENO);
            ::close(stderr_pipe[0]);
            ::close(stderr_pipe[1]);
        }

        if (stdin_pipe[0] >= 0) {
            ::dup2(stdin_pipe[0], STDIN_FILENO);
            ::close(stdin_pipe[0]);
            ::close(stdin_pipe[1]);
        }

        // Change working directory
        if (!merged.working_dir.empty()) {
            if (::chdir(merged.working_dir.c_str()) < 0)
                ::_exit(127);
        }

        // Build environment
        auto env_strings = build_env(merged);
        auto env_ptrs = std::vector<char*>{};
        env_ptrs.reserve(env_strings.size() + 1);
        for (auto& s : env_strings)
            env_ptrs.push_back(s.data());
        env_ptrs.push_back(nullptr);

        // Execute via shell
        auto cmd_str = std::string{command};
        char* const argv[] = {
            const_cast<char*>("/bin/sh"),
            const_cast<char*>("-c"),
            cmd_str.data(),
            nullptr
        };

        if (merged.inherit_env && merged.env.empty())
            ::execv("/bin/sh", argv);
        else
            ::execve("/bin/sh", argv, env_ptrs.data());

        ::_exit(127);
    }

    // Parent process
    if (stdout_pipe[1] >= 0)
        ::close(stdout_pipe[1]);
    if (stderr_pipe[1] >= 0)
        ::close(stderr_pipe[1]);
    if (stdin_pipe[0] >= 0)
        ::close(stdin_pipe[0]);

    // Write stdin data if provided
    if (merged.stdin_data && stdin_pipe[1] >= 0) {
        auto const& data = *merged.stdin_data;
        ::write(stdin_pipe[1], data.data(), data.size());
        ::close(stdin_pipe[1]);
        stdin_pipe[1] = -1;
    } else if (stdin_pipe[1] >= 0) {
        ::close(stdin_pipe[1]);
    }

    // Set pipes to non-blocking
    if (stdout_pipe[0] >= 0)
        ::fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
    if (stderr_pipe[0] >= 0)
        ::fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);

    auto result = CommandResult{};
    auto timed_out = false;

    // Read output with optional timeout
    auto deadline = merged.timeout
        ? std::optional{std::chrono::steady_clock::now() + *merged.timeout}
        : std::nullopt;

    auto buffer = std::array<char, 4096>{};
    auto stdout_open = stdout_pipe[0] >= 0;
    auto stderr_open = stderr_pipe[0] >= 0;

    while (stdout_open || stderr_open) {
        auto fds = std::array<pollfd, 2>{};
        auto nfds = 0;

        if (stdout_open) {
            fds[nfds].fd = stdout_pipe[0];
            fds[nfds].events = POLLIN;
            ++nfds;
        }
        if (stderr_open) {
            fds[nfds].fd = stderr_pipe[0];
            fds[nfds].events = POLLIN;
            ++nfds;
        }

        auto timeout_ms = -1;
        if (deadline) {
            auto remaining = *deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::milliseconds::zero()) {
                timed_out = true;
                break;
            }
            timeout_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count());
        }

        auto poll_result = ::poll(fds.data(), static_cast<nfds_t>(nfds), timeout_ms);
        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (poll_result == 0 && deadline) {
            timed_out = true;
            break;
        }

        for (auto i = 0; i < nfds; ++i) {
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                auto n = ::read(fds[i].fd, buffer.data(), buffer.size());
                if (n > 0) {
                    auto data = std::string_view{buffer.data(), static_cast<std::size_t>(n)};
                    auto is_stderr = (fds[i].fd == stderr_pipe[0]);

                    if (callback)
                        callback(data, is_stderr);

                    if (is_stderr)
                        result.stderr_output.append(data);
                    else
                        result.stdout_output.append(data);
                } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    if (fds[i].fd == stdout_pipe[0])
                        stdout_open = false;
                    else
                        stderr_open = false;
                }
            }
            if (fds[i].revents & (POLLERR | POLLNVAL)) {
                if (fds[i].fd == stdout_pipe[0])
                    stdout_open = false;
                else
                    stderr_open = false;
            }
        }
    }

    // Close remaining pipes
    if (stdout_pipe[0] >= 0)
        ::close(stdout_pipe[0]);
    if (stderr_pipe[0] >= 0)
        ::close(stderr_pipe[0]);

    // Handle timeout
    if (timed_out) {
        ::kill(pid, SIGKILL);
        result.timed_out = true;
    }

    // Wait for child
    auto status = 0;
    ::waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signaled = true;
        result.signal = WTERMSIG(status);
        result.exit_code = 128 + result.signal;
    }

    auto end_time = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);

    return result;
}

auto CommandRunner::merge_options(RunOptions const& options) const -> RunOptions
{
    auto merged = options;

    if (merged.working_dir.empty())
        merged.working_dir = default_options_.working_dir;

    // Merge environment variables
    auto env = default_options_.env;
    env.insert(env.end(), options.env.begin(), options.env.end());
    merged.env = std::move(env);

    if (!merged.timeout && default_options_.timeout)
        merged.timeout = default_options_.timeout;

    return merged;
}

auto CommandRunner::build_env(RunOptions const& options) const -> std::vector<std::string>
{
    auto result = std::vector<std::string>{};

    if (options.inherit_env) {
        for (auto** e = environ; *e != nullptr; ++e)
            result.emplace_back(*e);
    }

    for (auto const& var : options.env)
        result.push_back(var);

    return result;
}

auto parse_command(std::string_view command) -> std::vector<std::string>
{
    auto result = std::vector<std::string>{};
    auto current = std::string{};
    auto in_single_quote = false;
    auto in_double_quote = false;
    auto escape_next = false;

    for (auto c : command) {
        if (escape_next) {
            current += c;
            escape_next = false;
            continue;
        }

        if (c == '\\' && !in_single_quote) {
            escape_next = true;
            continue;
        }

        if (c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            continue;
        }

        if (c == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(c)) && !in_single_quote && !in_double_quote) {
            if (!current.empty()) {
                result.push_back(std::move(current));
                current.clear();
            }
            continue;
        }

        current += c;
    }

    if (!current.empty())
        result.push_back(std::move(current));

    return result;
}

auto shell_quote(std::string_view str) -> std::string
{
    auto needs_quoting = false;
    for (auto c : str) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' &&
            c != '.' && c != '/' && c != ':' && c != '@') {
            needs_quoting = true;
            break;
        }
    }

    if (!needs_quoting)
        return std::string{str};

    auto result = std::string{"'"};
    for (auto c : str) {
        if (c == '\'')
            result += "'\"'\"'";
        else
            result += c;
    }
    result += '\'';

    return result;
}

} // namespace pup::exec
