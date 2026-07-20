// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/sys.hpp"

#ifndef _WIN32

#    include <cerrno>
#    include <csignal>
#    include <cstdint>
#    include <cstdio>
#    include <cstdlib>
#    include <cstring>
#    include <ctime>

#    include <dirent.h>
#    include <fcntl.h>
#    include <poll.h>
#    include <sys/ioctl.h>
#    include <sys/mman.h>
#    include <sys/stat.h>
#    include <sys/wait.h>
#    include <unistd.h>

extern char** environ; // NOLINT

namespace pup::platform::sys {

namespace {

static_assert(sizeof(PollFd) == sizeof(pollfd));
static_assert(poll_in == POLLIN);
static_assert(poll_hup == POLLHUP);
static_assert(poll_err == POLLERR);

auto err() -> int
{
    return -errno;
}

auto or_errno(long r) -> int
{
    return r < 0 ? err() : static_cast<int>(r);
}

auto fill_stat(struct ::stat const& st, Stat& out) -> void
{
    out.mode = st.st_mode;
    out.size = static_cast<std::uint64_t>(st.st_size);
#    ifdef __APPLE__
    out.mtime_ns = std::int64_t { st.st_mtimespec.tv_sec } * 1'000'000'000 + st.st_mtimespec.tv_nsec;
#    else
    out.mtime_ns = std::int64_t { st.st_mtim.tv_sec } * 1'000'000'000 + st.st_mtim.tv_nsec;
#    endif
}

auto d_type_to_type(unsigned char dt) -> FileType
{
    switch (dt) {
    case DT_REG:
        return FileType::Regular;
    case DT_DIR:
        return FileType::Directory;
    case DT_LNK:
        return FileType::Symlink;
    case DT_UNKNOWN:
        return FileType::Unknown;
    default:
        return FileType::Other;
    }
}

} // namespace

auto open_ro(char const* path) -> int
{
    auto fd = ::open(path, O_RDONLY);
    return fd < 0 ? err() : fd;
}

auto create_trunc(char const* path, unsigned mode) -> int
{
    auto fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    return fd < 0 ? err() : fd;
}

auto close(int fd) -> int
{
    return or_errno(::close(fd));
}

auto read(int fd, void* buf, std::size_t n) -> std::int64_t
{
    auto r = ::read(fd, buf, n);
    return r < 0 ? err() : r;
}

auto write(int fd, void const* buf, std::size_t n) -> std::int64_t
{
    auto r = ::write(fd, buf, n);
    return r < 0 ? err() : r;
}

auto fsync(int fd) -> int
{
    return or_errno(::fsync(fd));
}

auto stat(char const* path, Stat& out) -> int
{
    struct ::stat st = {};
    if (::stat(path, &st) != 0) {
        return err();
    }
    fill_stat(st, out);
    return 0;
}

auto lstat(char const* path, Stat& out) -> int
{
    struct ::stat st = {};
    if (::lstat(path, &st) != 0) {
        return err();
    }
    fill_stat(st, out);
    return 0;
}

auto fstat(int fd, Stat& out) -> int
{
    struct ::stat st = {};
    if (::fstat(fd, &st) != 0) {
        return err();
    }
    fill_stat(st, out);
    return 0;
}

auto mkdir(char const* path, unsigned mode) -> int
{
    return or_errno(::mkdir(path, mode));
}

auto unlink(char const* path) -> int
{
    return or_errno(::unlink(path));
}

auto rmdir(char const* path) -> int
{
    return or_errno(::rmdir(path));
}

auto rename(char const* from, char const* to) -> int
{
    return or_errno(std::rename(from, to));
}

auto readlink(char const* path, char* buf, std::size_t n) -> std::int64_t
{
    auto r = ::readlink(path, buf, n);
    return r < 0 ? err() : r;
}

auto getcwd(char* buf, std::size_t n) -> std::int64_t
{
    if (::getcwd(buf, n) == nullptr) {
        return err();
    }
    return static_cast<std::int64_t>(std::strlen(buf));
}

auto chdir(char const* path) -> int
{
    return or_errno(::chdir(path));
}

auto open_dir(char const* path, Dir& d) -> int
{
    auto* dp = ::opendir(path);
    if (dp == nullptr) {
        return err();
    }
    d.handle = dp;
    return 0;
}

auto read_dir(Dir& d, DirEntry& out) -> int
{
    errno = 0;
    auto* e = ::readdir(static_cast<DIR*>(d.handle));
    if (e == nullptr) {
        return errno != 0 ? err() : 0;
    }
    out.name = e->d_name;
    out.type = d_type_to_type(e->d_type);
    return 1;
}

auto close_dir(Dir& d) -> int
{
    auto r = or_errno(::closedir(static_cast<DIR*>(d.handle)));
    d.handle = nullptr;
    return r;
}

auto reserve(std::size_t bytes) -> void*
{
    auto flags = MAP_PRIVATE | MAP_ANONYMOUS;
#    ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#    endif
    auto* p = ::mmap(nullptr, bytes, PROT_NONE, flags, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

auto remap_none(void* addr, std::size_t bytes) -> int
{
    auto* p = ::mmap(addr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? err() : 0;
}

auto commit(void* addr, std::size_t bytes) -> int
{
    return or_errno(::mprotect(addr, bytes, PROT_READ | PROT_WRITE));
}

auto map_file_ro(int fd, std::size_t bytes) -> void*
{
    auto* p = ::mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    return p == MAP_FAILED ? nullptr : p;
}

auto unmap(void* addr, std::size_t bytes) -> int
{
    return or_errno(::munmap(addr, bytes));
}

auto fork() -> std::int64_t
{
    auto pid = ::fork();
    return pid < 0 ? err() : pid;
}

auto execv(char const* path, char* const argv[]) -> int
{
    ::execv(path, argv);
    return err();
}

auto execve(char const* path, char* const argv[], char* const envp[]) -> int
{
    ::execve(path, argv, envp);
    return err();
}

auto wait_pid(std::int64_t pid, int& status, bool nohang) -> std::int64_t
{
    auto r = ::waitpid(static_cast<pid_t>(pid), &status, nohang ? WNOHANG : 0);
    return r < 0 ? err() : r;
}

auto kill(std::int64_t pid, int sig) -> int
{
    return or_errno(::kill(static_cast<pid_t>(pid), sig));
}

auto getpid() -> std::int64_t
{
    return ::getpid();
}

auto exit_process(int code) -> void
{
    ::_exit(code);
}

auto pipe(int fds[2]) -> int
{
    return or_errno(::pipe(fds));
}

auto dup2(int oldfd, int newfd) -> int
{
    auto r = ::dup2(oldfd, newfd);
    return r < 0 ? err() : r;
}

auto set_nonblocking(int fd) -> int
{
    auto flags = ::fcntl(fd, F_GETFL); // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (flags < 0) {
        return err();
    }
    return or_errno(::fcntl(fd, F_SETFL, flags | O_NONBLOCK)); // NOLINT(cppcoreguidelines-pro-type-vararg)
}

auto poll(PollFd* fds, unsigned count, int timeout_ms) -> int
{
    auto r = ::poll(reinterpret_cast<pollfd*>(fds), count, timeout_ms); // NOLINT
    return r < 0 ? err() : r;
}

auto getenv(char const* name) -> char const*
{
    return std::getenv(name);
}

auto cpu_count() -> int
{
    auto n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<int>(n) : 1;
}

auto page_size() -> std::size_t
{
    auto n = ::sysconf(_SC_PAGESIZE);
    return n > 0 ? static_cast<std::size_t>(n) : 4096;
}

auto clock_monotonic_ns() -> std::int64_t
{
    timespec ts = {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return std::int64_t { ts.tv_sec } * 1'000'000'000 + ts.tv_nsec;
}

auto clock_realtime_ns() -> std::int64_t
{
    timespec ts = {};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return std::int64_t { ts.tv_sec } * 1'000'000'000 + ts.tv_nsec;
}

auto isatty(int fd) -> bool
{
    return ::isatty(fd) != 0;
}

auto terminal_width(int fd) -> int
{
    winsize ws = {};
    if (::ioctl(fd, TIOCGWINSZ, &ws) != 0) { // NOLINT(cppcoreguidelines-pro-type-vararg)
        return 0;
    }
    return ws.ws_col;
}

} // namespace pup::platform::sys

#endif // !_WIN32
