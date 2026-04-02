// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
// POSIX APIs (pipe, fork, dup2, execve) require C-style arrays and pointer arithmetic

#include "pup/platform/async_process.hpp"

#include <cassert>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace pup::platform {

auto spawn_async(SpawnOptions const& opts) -> Result<AsyncProcess>
{
    int stdout_pipe[2] = { -1, -1 };
    int stderr_pipe[2] = { -1, -1 };

    if (::pipe(stdout_pipe) < 0) {
        return make_error<AsyncProcess>(ErrorCode::IoError, "Failed to create stdout pipe");
    }
    if (::pipe(stderr_pipe) < 0) {
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        return make_error<AsyncProcess>(ErrorCode::IoError, "Failed to create stderr pipe");
    }

    auto pid = ::fork();
    if (pid < 0) {
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        return make_error<AsyncProcess>(ErrorCode::IoError, "Failed to fork");
    }

    if (pid == 0) {
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);

        ::dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);

        if (!opts.working_dir.empty()) {
            if (::chdir(opts.working_dir.data()) < 0) {
                ::_exit(127);
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
            ::execve("/bin/sh", argv, opts.env);
        } else {
            ::execv("/bin/sh", argv);
        }

        ::_exit(127);
    }

    // Parent: close write ends, keep read ends
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);

    ::fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK); // NOLINT(cppcoreguidelines-pro-type-vararg)
    ::fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK); // NOLINT(cppcoreguidelines-pro-type-vararg)

    return AsyncProcess { .pid = pid, .stdout_fd = stdout_pipe[0], .stderr_fd = stderr_pipe[0] };
}

auto poll_fds(PollableFd* fds, std::size_t count, int timeout_ms) -> int
{
    if (count == 0) {
        return 0;
    }

    // Build pollfd array on stack for typical sizes, heap for large
    constexpr auto stack_limit = std::size_t { 64 };
    pollfd stack_buf[stack_limit]; // NOLINT(modernize-avoid-c-arrays)
    auto* pfds = count <= stack_limit ? stack_buf : new pollfd[count]; // NOLINT

    for (auto i = std::size_t { 0 }; i < count; ++i) {
        pfds[i].fd = fds[i].fd;
        pfds[i].events = POLLIN;
        pfds[i].revents = 0;
    }

    int result;
    do {
        result = ::poll(pfds, static_cast<nfds_t>(count), timeout_ms);
    } while (result < 0 && errno == EINTR);

    // Propagate revents back: callers check POLLIN/POLLHUP via the return value
    // but the fd array indices match, so just store revents for the caller
    // Actually, caller just needs to know which fds are ready. The return value
    // from ::poll is sufficient -- but we also need to know *which* fds triggered.
    // We repurpose the fd field: set it to -1 for non-ready fds so caller can skip.
    if (result > 0) {
        for (auto i = std::size_t { 0 }; i < count; ++i) {
            if (!(pfds[i].revents & (POLLIN | POLLHUP))) {
                fds[i].fd = -1; // mark not ready
            }
        }
    }

    if (count > stack_limit) {
        delete[] pfds; // NOLINT
    }

    return result;
}

auto read_nonblocking(int fd, char* buf, std::size_t size) -> int
{
    auto n = ::read(fd, buf, size);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return -1;
    }
    return static_cast<int>(n);
}

auto close_fd(int fd) -> void
{
    ::close(fd);
}

auto try_reap(int pid, ProcessStatus& out) -> bool
{
    auto status = 0;
    auto wpid = ::waitpid(pid, &status, WNOHANG);
    if (wpid <= 0) {
        return false;
    }

    out.exited = true;
    if (WIFEXITED(status)) {
        out.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        out.exit_code = 128 + WTERMSIG(status);
    }
    return true;
}

auto reap(int pid, ProcessStatus& out) -> void
{
    auto status = 0;
    ::waitpid(pid, &status, 0);

    out.exited = true;
    if (WIFEXITED(status)) {
        out.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        out.exit_code = 128 + WTERMSIG(status);
    }
}

auto send_signal(int pid, Signal sig) -> void
{
    assert(pid > 0 && "send_signal called with invalid pid");
    ::kill(pid, sig == Signal::Kill ? SIGKILL : SIGTERM);
}

} // namespace pup::platform

// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
// NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
