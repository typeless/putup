// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
// POSIX APIs (pipe, execv, environ) require C-style arrays and pointer arithmetic

#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/process.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#    include <crt_externs.h>
#    define environ (*_NSGetEnviron())
#endif

namespace pup::platform {

auto build_env_strings(
    Vec<StringId> const& extra_env,
    bool inherit_env
) -> Vec<StringId>
{
    auto& pool = global_pool();
    auto result = Vec<StringId> {};

    if (inherit_env) {
        for (auto** e = environ; *e != nullptr; ++e) {
            result.push_back(pool.intern(*e));
        }
    }

    for (auto var : extra_env) {
        result.push_back(var);
    }

    return result;
}

namespace {

auto close_pipe(int pipe_fd[2]) -> void
{
    if (pipe_fd[0] >= 0) {
        ::close(pipe_fd[0]);
        pipe_fd[0] = -1;
    }
    if (pipe_fd[1] >= 0) {
        ::close(pipe_fd[1]);
        pipe_fd[1] = -1;
    }
}

} // namespace

auto run_process(ProcessOptions const& opts) -> Result<ProcessResult>
{
    return run_process_with_callback(opts, nullptr, nullptr);
}

auto run_process_with_callback(
    ProcessOptions const& opts,
    ProcessOutputCallback callback,
    void* user_data
) -> Result<ProcessResult>
{
    auto& pool = global_pool();
    auto start_time = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };

    int stdout_pipe[2] = { -1, -1 };
    int stderr_pipe[2] = { -1, -1 };
    int stdin_pipe[2] = { -1, -1 };

    if (opts.capture_stdout && ::pipe(stdout_pipe) < 0) {
        return make_error<ProcessResult>(ErrorCode::IoError, "Failed to create stdout pipe");
    }

    if (opts.capture_stderr && ::pipe(stderr_pipe) < 0) {
        close_pipe(stdout_pipe);
        return make_error<ProcessResult>(ErrorCode::IoError, "Failed to create stderr pipe");
    }

    if (opts.stdin_data && ::pipe(stdin_pipe) < 0) {
        close_pipe(stdout_pipe);
        close_pipe(stderr_pipe);
        return make_error<ProcessResult>(ErrorCode::IoError, "Failed to create stdin pipe");
    }

    auto pid = pid_t { ::fork() };
    if (pid < 0) {
        close_pipe(stdout_pipe);
        close_pipe(stderr_pipe);
        close_pipe(stdin_pipe);
        return make_error<ProcessResult>(ErrorCode::IoError, "Failed to fork");
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

        auto working_dir = pool.get(opts.working_dir);
        if (!working_dir.empty()) {
            if (::chdir(working_dir.data()) < 0) {
                ::_exit(127);
            }
        }

        auto env_strings = build_env_strings(opts.env, opts.inherit_env);
        auto env_ptrs = Vec<char*> {};
        env_ptrs.reserve(env_strings.size() + 1);
        for (auto s : env_strings) {
            // pool.get() returns null-terminated data
            env_ptrs.push_back(const_cast<char*>(pool.get(s).data())); // NOLINT(cppcoreguidelines-pro-type-const-cast)
        }
        env_ptrs.push_back(nullptr);

        char* const argv[] = {
            const_cast<char*>("/bin/sh"), // NOLINT(cppcoreguidelines-pro-type-const-cast)
            const_cast<char*>("-c"),      // NOLINT(cppcoreguidelines-pro-type-const-cast)
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) - POSIX exec requires char*
            const_cast<char*>(pool.get(opts.command).data()),
            nullptr
        };

        if (opts.inherit_env && opts.env.empty()) {
            ::execv("/bin/sh", argv);
        } else {
            ::execve("/bin/sh", argv, env_ptrs.data());
        }

        ::_exit(127);
    }

    // Parent process
    if (stdout_pipe[1] >= 0) {
        ::close(stdout_pipe[1]);
        stdout_pipe[1] = -1;
    }
    if (stderr_pipe[1] >= 0) {
        ::close(stderr_pipe[1]);
        stderr_pipe[1] = -1;
    }
    if (stdin_pipe[0] >= 0) {
        ::close(stdin_pipe[0]);
        stdin_pipe[0] = -1;
    }

    if (opts.stdin_data && stdin_pipe[1] >= 0) {
        auto const& data = *opts.stdin_data;
        auto written = ::write(stdin_pipe[1], data.data(), data.size());
        (void)written;
        ::close(stdin_pipe[1]);
        stdin_pipe[1] = -1;
    } else if (stdin_pipe[1] >= 0) {
        ::close(stdin_pipe[1]);
        stdin_pipe[1] = -1;
    }

    if (stdout_pipe[0] >= 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        ::fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
    }
    if (stderr_pipe[0] >= 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        ::fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);
    }

    auto result = ProcessResult {};
    auto timed_out = false;
    auto stdout_buf = String {};
    auto stderr_buf = String {};

    auto deadline = opts.timeout
        ? std::optional { std::chrono::steady_clock::now() + *opts.timeout }
        : std::nullopt;

    auto buffer = std::array<char, 4096> {};
    auto stdout_open = stdout_pipe[0] >= 0;
    auto stderr_open = stderr_pipe[0] >= 0;

    while (stdout_open || stderr_open) {
        auto fds = std::array<pollfd, 2> {};
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
                std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count()
            );
        }

        auto poll_result = ::poll(fds.data(), static_cast<nfds_t>(nfds), timeout_ms);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
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
                    auto data = std::string_view { buffer.data(), static_cast<std::size_t>(n) };
                    auto is_stderr = (fds[i].fd == stderr_pipe[0]);

                    if (callback) {
                        callback(data, is_stderr, user_data);
                    }

                    if (is_stderr) {
                        stderr_buf.append(data);
                    } else {
                        stdout_buf.append(data);
                    }
                } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    if (fds[i].fd == stdout_pipe[0]) {
                        stdout_open = false;
                    } else {
                        stderr_open = false;
                    }
                }
            }
            if (fds[i].revents & (POLLERR | POLLNVAL)) {
                if (fds[i].fd == stdout_pipe[0]) {
                    stdout_open = false;
                } else {
                    stderr_open = false;
                }
            }
        }
    }

    if (stdout_pipe[0] >= 0) {
        ::close(stdout_pipe[0]);
    }
    if (stderr_pipe[0] >= 0) {
        ::close(stderr_pipe[0]);
    }

    if (timed_out) {
        ::kill(pid, SIGKILL);
        result.timed_out = true;
    }

    auto status = 0;
    ::waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signaled = true;
        result.signal = WTERMSIG(status);
        result.exit_code = 128 + result.signal;
    }

    auto end_time = std::chrono::steady_clock::time_point { std::chrono::steady_clock::now() };
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time
    );

    result.stdout_output = pool.intern(stdout_buf);
    result.stderr_output = pool.intern(stderr_buf);

    return result;
}

} // namespace pup::platform

// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
// NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
