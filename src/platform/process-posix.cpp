// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
// POSIX APIs (pipe, execv, environ) require C-style arrays and pointer arithmetic

#include "pup/core/buf.hpp"
#include "pup/core/clock.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/process.hpp"
#include "pup/platform/sys.hpp"

#include <array>
#include <cassert>
#include <cerrno>
#include <csignal>

#ifdef __APPLE__
#    include <crt_externs.h>
#    define environ (*_NSGetEnviron())
#else
extern char** environ; // NOLINT
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

auto base_child_env() -> Vec<StringId>
{
    auto& pool = global_pool();
    auto result = Vec<StringId> {};
    auto buf = Buf {};
    buf.append(std::string_view { "PATH=" });
    if (auto const* path = sys::getenv("PATH")) {
        buf.append(std::string_view { path });
    } else {
        buf.append(std::string_view { "/usr/bin:/bin" });
    }
    result.push_back(buf.intern(pool));
    return result;
}

namespace {

auto close_pipe(int pipe_fd[2]) -> void
{
    if (pipe_fd[0] >= 0) {
        sys::close(pipe_fd[0]);
        pipe_fd[0] = -1;
    }
    if (pipe_fd[1] >= 0) {
        sys::close(pipe_fd[1]);
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
    auto start_time = pup::SteadyClock::time_point { pup::SteadyClock::now() };

    int stdout_pipe[2] = { -1, -1 };
    int stderr_pipe[2] = { -1, -1 };
    int stdin_pipe[2] = { -1, -1 };

    if (opts.capture_stdout && sys::pipe(stdout_pipe) < 0) {
        return make_error<ProcessResult>(ErrorCode::IoError, "Failed to create stdout pipe");
    }

    if (opts.capture_stderr && sys::pipe(stderr_pipe) < 0) {
        close_pipe(stdout_pipe);
        return make_error<ProcessResult>(ErrorCode::IoError, "Failed to create stderr pipe");
    }

    if (opts.stdin_data && sys::pipe(stdin_pipe) < 0) {
        close_pipe(stdout_pipe);
        close_pipe(stderr_pipe);
        return make_error<ProcessResult>(ErrorCode::IoError, "Failed to create stdin pipe");
    }

    auto pid = sys::fork();
    if (pid < 0) {
        close_pipe(stdout_pipe);
        close_pipe(stderr_pipe);
        close_pipe(stdin_pipe);
        return make_error<ProcessResult>(ErrorCode::IoError, "Failed to fork");
    }

    if (pid == 0) {
        // Child process
        if (stdout_pipe[1] >= 0) {
            sys::dup2(stdout_pipe[1], 1);
            sys::close(stdout_pipe[0]);
            sys::close(stdout_pipe[1]);
        }

        if (stderr_pipe[1] >= 0) {
            sys::dup2(stderr_pipe[1], 2);
            sys::close(stderr_pipe[0]);
            sys::close(stderr_pipe[1]);
        }

        if (stdin_pipe[0] >= 0) {
            sys::dup2(stdin_pipe[0], 0);
            sys::close(stdin_pipe[0]);
            sys::close(stdin_pipe[1]);
        }

        auto working_dir = pool.get(opts.working_dir);
        if (!working_dir.empty()) {
            if (sys::chdir(working_dir.data()) < 0) {
                sys::exit_process(127);
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
            sys::execv("/bin/sh", argv);
        } else {
            sys::execve("/bin/sh", argv, env_ptrs.data());
        }

        sys::exit_process(127);
    }

    // Parent process
    if (stdout_pipe[1] >= 0) {
        sys::close(stdout_pipe[1]);
        stdout_pipe[1] = -1;
    }
    if (stderr_pipe[1] >= 0) {
        sys::close(stderr_pipe[1]);
        stderr_pipe[1] = -1;
    }
    if (stdin_pipe[0] >= 0) {
        sys::close(stdin_pipe[0]);
        stdin_pipe[0] = -1;
    }

    if (opts.stdin_data && stdin_pipe[1] >= 0) {
        auto data = global_pool().get(*opts.stdin_data);
        auto written = sys::write(stdin_pipe[1], data.data(), data.size());
        (void)written;
        sys::close(stdin_pipe[1]);
        stdin_pipe[1] = -1;
    } else if (stdin_pipe[1] >= 0) {
        sys::close(stdin_pipe[1]);
        stdin_pipe[1] = -1;
    }

    if (stdout_pipe[0] >= 0) {
        sys::set_nonblocking(stdout_pipe[0]);
    }
    if (stderr_pipe[0] >= 0) {
        sys::set_nonblocking(stderr_pipe[0]);
    }

    auto result = ProcessResult {};
    auto timed_out = false;
    auto stdout_buf = Buf {};
    auto stderr_buf = Buf {};

    auto deadline = opts.timeout
        ? std::optional { pup::SteadyClock::now() + *opts.timeout }
        : std::nullopt;

    auto buffer = std::array<char, 4096> {};
    auto stdout_open = stdout_pipe[0] >= 0;
    auto stderr_open = stderr_pipe[0] >= 0;

    while (stdout_open || stderr_open) {
        auto fds = std::array<sys::PollFd, 2> {};
        auto nfds = 0;

        if (stdout_open) {
            fds[nfds].fd = stdout_pipe[0];
            fds[nfds].events = sys::poll_in;
            ++nfds;
        }
        if (stderr_open) {
            fds[nfds].fd = stderr_pipe[0];
            fds[nfds].events = sys::poll_in;
            ++nfds;
        }

        auto timeout_ms = -1;
        if (deadline) {
            auto remaining = *deadline - pup::SteadyClock::now();
            if (remaining <= std::chrono::milliseconds::zero()) {
                timed_out = true;
                break;
            }
            timeout_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count()
            );
        }

        auto poll_result = sys::poll(fds.data(), static_cast<unsigned>(nfds), timeout_ms);
        if (poll_result < 0) {
            if (poll_result == -EINTR) {
                continue;
            }
            break;
        }

        if (poll_result == 0 && deadline) {
            timed_out = true;
            break;
        }

        for (auto i = 0; i < nfds; ++i) {
            if (fds[i].revents & (sys::poll_in | sys::poll_hup)) {
                auto n = sys::read(fds[i].fd, buffer.data(), buffer.size());
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
                } else if (n == 0 || (n < 0 && n != -EAGAIN && n != -EWOULDBLOCK)) {
                    if (fds[i].fd == stdout_pipe[0]) {
                        stdout_open = false;
                    } else {
                        stderr_open = false;
                    }
                }
            }
            if (fds[i].revents & (sys::poll_err | sys::poll_nval)) {
                if (fds[i].fd == stdout_pipe[0]) {
                    stdout_open = false;
                } else {
                    stderr_open = false;
                }
            }
        }
    }

    if (stdout_pipe[0] >= 0) {
        sys::close(stdout_pipe[0]);
    }
    if (stderr_pipe[0] >= 0) {
        sys::close(stderr_pipe[0]);
    }

    if (timed_out) {
        sys::kill(pid, SIGKILL);
        result.timed_out = true;
    }

    auto status = 0;
    sys::wait_pid(pid, status, false);

    if (sys::exited(status)) {
        result.exit_code = sys::exit_code(status);
    } else if (sys::signaled(status)) {
        result.signaled = true;
        result.signal = sys::term_signal(status);
        result.exit_code = 128 + result.signal;
    }

    auto end_time = pup::SteadyClock::time_point { pup::SteadyClock::now() };
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time
    );

    result.stdout_output = pool.intern(stdout_buf.view());
    result.stderr_output = pool.intern(stderr_buf.view());

    return result;
}

auto run_parallel_tasks(
    int (*task)(void* ctx),
    void** contexts,
    std::size_t count
) -> int
{
    if (count == 0) {
        return 0;
    }

    // Fork one child per task
    auto pids = Vec<std::int64_t> {};
    pids.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        auto pid = sys::fork();
        if (pid < 0) {
            // Fork failed — run remaining tasks sequentially in parent
            auto failed = 0;
            for (auto j = i; j < count; ++j) {
                if (task(contexts[j]) != 0) {
                    ++failed;
                }
            }
            // Reap already-forked children
            for (auto child : pids) {
                auto status = 0;
                sys::wait_pid(child, status, false);
                if (!sys::exited(status) || sys::exit_code(status) != 0) {
                    ++failed;
                }
            }
            return failed;
        }

        if (pid == 0) {
            // Child: run task and exit with its return code. No stdio to flush —
            // pup::print writes are unbuffered raw syscalls.
            auto rc = task(contexts[i]);
            sys::exit_process(rc);
        }

        pids.push_back(pid);
    }

    // Parent: wait for all children
    auto failed = 0;
    for (auto pid : pids) {
        auto status = 0;
        sys::wait_pid(pid, status, false);
        if (!sys::exited(status) || sys::exit_code(status) != 0) {
            ++failed;
        }
    }
    return failed;
}

auto spawn_async(SpawnOptions const& opts) -> Result<AsyncProcess>
{
    int stdout_pipe[2] = { -1, -1 };
    int stderr_pipe[2] = { -1, -1 };

    if (sys::pipe(stdout_pipe) < 0) {
        return make_error<AsyncProcess>(ErrorCode::IoError, "Failed to create stdout pipe");
    }
    if (sys::pipe(stderr_pipe) < 0) {
        sys::close(stdout_pipe[0]);
        sys::close(stdout_pipe[1]);
        return make_error<AsyncProcess>(ErrorCode::IoError, "Failed to create stderr pipe");
    }

    auto pid = sys::fork();
    if (pid < 0) {
        sys::close(stdout_pipe[0]);
        sys::close(stdout_pipe[1]);
        sys::close(stderr_pipe[0]);
        sys::close(stderr_pipe[1]);
        return make_error<AsyncProcess>(ErrorCode::IoError, "Failed to fork");
    }

    if (pid == 0) {
        sys::dup2(stdout_pipe[1], 1);
        sys::close(stdout_pipe[0]);
        sys::close(stdout_pipe[1]);

        sys::dup2(stderr_pipe[1], 2);
        sys::close(stderr_pipe[0]);
        sys::close(stderr_pipe[1]);

        if (!opts.working_dir.empty()) {
            if (sys::chdir(opts.working_dir.data()) < 0) {
                sys::exit_process(127);
            }
        }

        // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast)
        char* const argv[] = {
            const_cast<char*>("/bin/sh"),
            const_cast<char*>("-c"),
            const_cast<char*>(opts.command.data()),
            nullptr
        };
        // NOLINTEND(cppcoreguidelines-pro-type-const-cast)

        if (opts.env) {
            sys::execve("/bin/sh", argv, opts.env);
        } else {
            sys::execv("/bin/sh", argv);
        }

        sys::exit_process(127);
    }

    // Parent: close write ends, keep read ends
    sys::close(stdout_pipe[1]);
    sys::close(stderr_pipe[1]);

    sys::set_nonblocking(stdout_pipe[0]);
    sys::set_nonblocking(stderr_pipe[0]);

    return AsyncProcess {
        .pid = static_cast<std::intptr_t>(pid),
        .stdout_fd = static_cast<std::intptr_t>(stdout_pipe[0]),
        .stderr_fd = static_cast<std::intptr_t>(stderr_pipe[0]),
    };
}

auto poll_fds(PollableFd* fds, std::size_t count, int timeout_ms) -> int
{
    if (count == 0) {
        return 0;
    }

    // Build poll array on stack for typical sizes, heap for large
    constexpr auto stack_limit = std::size_t { 64 };
    sys::PollFd stack_buf[stack_limit];                                     // NOLINT(modernize-avoid-c-arrays)
    auto* pfds = count <= stack_limit ? stack_buf : new sys::PollFd[count]; // NOLINT

    for (auto i = std::size_t { 0 }; i < count; ++i) {
        pfds[i].fd = static_cast<int>(fds[i].fd);
        pfds[i].events = sys::poll_in;
        pfds[i].revents = 0;
    }

    int result;
    do {
        result = sys::poll(pfds, static_cast<unsigned>(count), timeout_ms);
    } while (result == -EINTR);

    if (result > 0) {
        for (auto i = std::size_t { 0 }; i < count; ++i) {
            if (!(pfds[i].revents & (sys::poll_in | sys::poll_hup))) {
                fds[i].fd = -1; // mark not ready
            }
        }
    }

    if (count > stack_limit) {
        delete[] pfds; // NOLINT
    }

    return result;
}

auto read_nonblocking(std::intptr_t fd, char* buf, std::size_t size) -> int
{
    auto n = sys::read(static_cast<int>(fd), buf, size);
    if (n == -EAGAIN || n == -EWOULDBLOCK) {
        return -1;
    }
    return static_cast<int>(n);
}

auto close_fd(std::intptr_t fd) -> void
{
    sys::close(static_cast<int>(fd));
}

auto try_reap(std::intptr_t pid, ProcessStatus& out) -> bool
{
    auto status = 0;
    auto wpid = sys::wait_pid(pid, status, true);
    if (wpid <= 0) {
        return false;
    }

    out.exited = true;
    if (sys::exited(status)) {
        out.exit_code = sys::exit_code(status);
    } else if (sys::signaled(status)) {
        out.exit_code = 128 + sys::term_signal(status);
    }
    return true;
}

auto reap(std::intptr_t pid, ProcessStatus& out) -> void
{
    auto status = 0;
    sys::wait_pid(pid, status, false);

    out.exited = true;
    if (sys::exited(status)) {
        out.exit_code = sys::exit_code(status);
    } else if (sys::signaled(status)) {
        out.exit_code = 128 + sys::term_signal(status);
    }
}

auto send_signal(std::intptr_t pid, Signal sig) -> void
{
    assert(pid > 0 && "send_signal called with invalid pid");
    sys::kill(pid, sig == Signal::Kill ? SIGKILL : SIGTERM);
}

} // namespace pup::platform

// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
// NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
