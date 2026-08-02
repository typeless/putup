// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstddef>
#include <cstdint>

namespace pup::platform::sys {

// Kernel convention throughout: failures return -errno; successes return >= 0.
// No global errno.

constexpr auto path_max = std::size_t { 4096 };

struct Stat {
    std::uint32_t mode = 0;
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
};

constexpr auto is_dir(std::uint32_t mode) -> bool
{
    return (mode & 0170000U) == 0040000U;
}

constexpr auto is_reg(std::uint32_t mode) -> bool
{
    return (mode & 0170000U) == 0100000U;
}

constexpr auto is_lnk(std::uint32_t mode) -> bool
{
    return (mode & 0170000U) == 0120000U;
}

auto open_ro(char const* path) -> int;
auto create_trunc(char const* path, unsigned mode) -> int;
auto close(int fd) -> int;
auto read(int fd, void* buf, std::size_t n) -> std::int64_t;
auto write(int fd, void const* buf, std::size_t n) -> std::int64_t;
auto fsync(int fd) -> int;

auto stat(char const* path, Stat& out) -> int;
auto lstat(char const* path, Stat& out) -> int;
auto fstat(int fd, Stat& out) -> int;

auto mkdir(char const* path, unsigned mode) -> int;
auto unlink(char const* path) -> int;
auto rmdir(char const* path) -> int;
auto rename(char const* from, char const* to) -> int;
auto readlink(char const* path, char* buf, std::size_t n) -> std::int64_t;
auto getcwd(char* buf, std::size_t n) -> std::int64_t;
auto chdir(char const* path) -> int;
auto realpath(char const* path, char (&out)[path_max]) -> std::int64_t;

/// Text for an errno these calls return (sign-insensitive); null when unrecognised.
/// Returns char const* so this header pulls in no std headers: it is included by
/// throw_stubs.cpp, which defines the std::__throw_* symbols libstdc++ would declare.
auto error_text(int err) -> char const*;

enum class FileType : std::uint8_t { Unknown,
                                     Regular,
                                     Directory,
                                     Symlink,
                                     Other };

struct DirEntry {
    char const* name = nullptr;
    FileType type = FileType::Unknown;
};

struct Dir {
    void* handle = nullptr;
    int fd = -1;
    std::size_t pos = 0;
    std::size_t len = 0;
    char buf[4096] = {};
};

auto open_dir(char const* path, Dir& d) -> int;
auto read_dir(Dir& d, DirEntry& out) -> int;
auto close_dir(Dir& d) -> int;

auto reserve(std::size_t bytes) -> void*;
auto remap_none(void* addr, std::size_t bytes) -> int;
auto commit(void* addr, std::size_t bytes) -> int;
auto map_file_ro(int fd, std::size_t bytes) -> void*;
auto unmap(void* addr, std::size_t bytes) -> int;

auto fork() -> std::int64_t;
auto execv(char const* path, char* const argv[]) -> int;
auto execve(char const* path, char* const argv[], char* const envp[]) -> int;
auto wait_pid(std::int64_t pid, int& status, bool nohang) -> std::int64_t;
auto kill(std::int64_t pid, int sig) -> int;
auto getpid() -> std::int64_t;
[[noreturn]]
auto exit_process(int code) -> void;

constexpr auto exited(int status) -> bool
{
    return (status & 0x7f) == 0;
}

constexpr auto exit_code(int status) -> int
{
    return (status >> 8) & 0xff;
}

constexpr auto signaled(int status) -> bool
{
    return ((status & 0x7f) + 1) >> 1 > 0;
}

constexpr auto term_signal(int status) -> int
{
    return status & 0x7f;
}

struct PollFd {
    int fd;
    short events;
    short revents;
};

constexpr auto poll_in = short { 0x001 };
constexpr auto poll_hup = short { 0x010 };
constexpr auto poll_err = short { 0x008 };
constexpr auto poll_nval = short { 0x020 };

auto pipe(int fds[2]) -> int;
auto dup2(int oldfd, int newfd) -> int;
auto set_nonblocking(int fd) -> int;
auto poll(PollFd* fds, unsigned count, int timeout_ms) -> int;

auto getenv(char const* name) -> char const*;
auto cpu_count() -> int;
auto page_size() -> std::size_t;
auto clock_monotonic_ns() -> std::int64_t;
auto clock_realtime_ns() -> std::int64_t;
auto isatty(int fd) -> bool;
auto terminal_width(int fd) -> int;

} // namespace pup::platform::sys
