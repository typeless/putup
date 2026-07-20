// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/sys.hpp"

#if defined(__linux__) && defined(__x86_64__)

#    include <cerrno>
#    include <csignal>
#    include <cstddef>
#    include <cstdint>
#    include <cstring>
#    include <ctime>

#    include <asm/unistd.h> // IWYU pragma: keep — __NR_* syscall numbers
#    include <dirent.h>     // IWYU pragma: keep — DT_* macros
#    include <fcntl.h>
#    include <linux/auxvec.h>
#    include <sys/ioctl.h>
#    include <sys/mman.h>
#    include <sys/stat.h> // IWYU pragma: keep — struct statx, STATX_* macros
#    include <sys/wait.h>

extern char** environ; // NOLINT

namespace pup::platform::sys {

namespace {

auto raw(long nr, long a1 = 0, long a2 = 0, long a3 = 0, long a4 = 0, long a5 = 0, long a6 = 0) -> long
{
    register long r10 asm("r10") = a4; // NOLINT
    register long r8 asm("r8") = a5;   // NOLINT
    register long r9 asm("r9") = a6;   // NOLINT
    long ret = 0;                      // NOLINT
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return ret;
}

auto arg(void const* p) -> long
{
    return reinterpret_cast<long>(p); // NOLINT
}

auto map_result(long r) -> void*
{
    if (static_cast<unsigned long>(r) >= static_cast<unsigned long>(-4095L)) {
        return nullptr;
    }
    return reinterpret_cast<void*>(r); // NOLINT
}

auto fill_stat(struct statx const& stx, Stat& out) -> void
{
    out.mode = stx.stx_mode;
    out.size = stx.stx_size;
    out.mtime_ns = std::int64_t { stx.stx_mtime.tv_sec } * 1'000'000'000 + stx.stx_mtime.tv_nsec;
}

auto statx_at(long dirfd, char const* path, long flags, Stat& out) -> int
{
    struct statx stx = {};
    auto r = raw(__NR_statx, dirfd, arg(path), flags, STATX_MODE | STATX_SIZE | STATX_MTIME, arg(&stx));
    if (r == 0) {
        fill_stat(stx, out);
    }
    return static_cast<int>(r);
}

struct RawDirent64 {
    std::uint64_t ino;
    std::int64_t off;
    std::uint16_t reclen;
    std::uint8_t type;
    char name[1];
};

auto dt_to_type(std::uint8_t dt) -> FileType
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

constexpr auto TCGETS_REQUEST = 0x5401L;

} // namespace

auto open_ro(char const* path) -> int
{
    return static_cast<int>(raw(__NR_openat, AT_FDCWD, arg(path), O_RDONLY));
}

auto create_trunc(char const* path, unsigned mode) -> int
{
    return static_cast<int>(raw(__NR_openat, AT_FDCWD, arg(path), O_WRONLY | O_CREAT | O_TRUNC, static_cast<long>(mode)));
}

auto close(int fd) -> int
{
    return static_cast<int>(raw(__NR_close, fd));
}

auto read(int fd, void* buf, std::size_t n) -> std::int64_t
{
    return raw(__NR_read, fd, arg(buf), static_cast<long>(n));
}

auto write(int fd, void const* buf, std::size_t n) -> std::int64_t
{
    return raw(__NR_write, fd, arg(buf), static_cast<long>(n));
}

auto fsync(int fd) -> int
{
    return static_cast<int>(raw(__NR_fsync, fd));
}

auto stat(char const* path, Stat& out) -> int
{
    return statx_at(AT_FDCWD, path, 0, out);
}

auto lstat(char const* path, Stat& out) -> int
{
    return statx_at(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, out);
}

auto fstat(int fd, Stat& out) -> int
{
    return statx_at(fd, "", AT_EMPTY_PATH, out);
}

auto mkdir(char const* path, unsigned mode) -> int
{
    return static_cast<int>(raw(__NR_mkdirat, AT_FDCWD, arg(path), static_cast<long>(mode)));
}

auto unlink(char const* path) -> int
{
    return static_cast<int>(raw(__NR_unlinkat, AT_FDCWD, arg(path), 0));
}

auto rmdir(char const* path) -> int
{
    return static_cast<int>(raw(__NR_unlinkat, AT_FDCWD, arg(path), AT_REMOVEDIR));
}

auto rename(char const* from, char const* to) -> int
{
    return static_cast<int>(raw(__NR_renameat2, AT_FDCWD, arg(from), AT_FDCWD, arg(to), 0));
}

auto readlink(char const* path, char* buf, std::size_t n) -> std::int64_t
{
    return raw(__NR_readlinkat, AT_FDCWD, arg(path), arg(buf), static_cast<long>(n));
}

auto getcwd(char* buf, std::size_t n) -> std::int64_t
{
    auto r = raw(__NR_getcwd, arg(buf), static_cast<long>(n));
    return r > 0 ? r - 1 : r;
}

auto chdir(char const* path) -> int
{
    return static_cast<int>(raw(__NR_chdir, arg(path)));
}

auto open_dir(char const* path, Dir& d) -> int
{
    auto fd = static_cast<int>(raw(__NR_openat, AT_FDCWD, arg(path), O_RDONLY | O_DIRECTORY));
    if (fd < 0) {
        return fd;
    }
    d.fd = fd;
    d.pos = 0;
    d.len = 0;
    return 0;
}

auto read_dir(Dir& d, DirEntry& out) -> int
{
    if (d.pos >= d.len) {
        auto r = raw(__NR_getdents64, d.fd, arg(d.buf), static_cast<long>(sizeof(d.buf)));
        if (r <= 0) {
            return static_cast<int>(r);
        }
        d.len = static_cast<std::size_t>(r);
        d.pos = 0;
    }
    auto const* e = reinterpret_cast<RawDirent64 const*>(d.buf + d.pos); // NOLINT
    out.name = e->name;
    out.type = dt_to_type(e->type);
    d.pos += e->reclen;
    return 1;
}

auto close_dir(Dir& d) -> int
{
    auto r = close(d.fd);
    d.fd = -1;
    return r;
}

auto reserve(std::size_t bytes) -> void*
{
    return map_result(raw(__NR_mmap, 0, static_cast<long>(bytes), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
}

auto remap_none(void* addr, std::size_t bytes) -> int
{
    auto r = raw(__NR_mmap, arg(addr), static_cast<long>(bytes), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return map_result(r) == nullptr ? static_cast<int>(r) : 0;
}

auto commit(void* addr, std::size_t bytes) -> int
{
    return static_cast<int>(raw(__NR_mprotect, arg(addr), static_cast<long>(bytes), PROT_READ | PROT_WRITE));
}

auto map_file_ro(int fd, std::size_t bytes) -> void*
{
    return map_result(raw(__NR_mmap, 0, static_cast<long>(bytes), PROT_READ, MAP_PRIVATE, fd, 0));
}

auto unmap(void* addr, std::size_t bytes) -> int
{
    return static_cast<int>(raw(__NR_munmap, arg(addr), static_cast<long>(bytes)));
}

// clone(SIGCHLD) is fork; raw bypass of glibc fork machinery is safe only
// while the process stays single-threaded and allocates off the bump region.
auto fork() -> std::int64_t
{
    return raw(__NR_clone, SIGCHLD, 0, 0, 0, 0);
}

auto execv(char const* path, char* const argv[]) -> int
{
    return execve(path, argv, environ);
}

auto execve(char const* path, char* const argv[], char* const envp[]) -> int
{
    return static_cast<int>(raw(__NR_execve, arg(path), arg(argv), arg(envp)));
}

auto wait_pid(std::int64_t pid, int& status, bool nohang) -> std::int64_t
{
    return raw(__NR_wait4, static_cast<long>(pid), arg(&status), nohang ? WNOHANG : 0, 0);
}

auto kill(std::int64_t pid, int sig) -> int
{
    return static_cast<int>(raw(__NR_kill, static_cast<long>(pid), sig));
}

auto getpid() -> std::int64_t
{
    return raw(__NR_getpid);
}

auto exit_process(int code) -> void
{
    raw(__NR_exit_group, code);
    __builtin_unreachable();
}

auto pipe(int fds[2]) -> int
{
    return static_cast<int>(raw(__NR_pipe2, arg(fds), 0));
}

auto dup2(int oldfd, int newfd) -> int
{
    if (oldfd == newfd) {
        auto r = raw(__NR_fcntl, oldfd, F_GETFL);
        return r < 0 ? static_cast<int>(r) : newfd;
    }
    return static_cast<int>(raw(__NR_dup3, oldfd, newfd, 0));
}

auto set_nonblocking(int fd) -> int
{
    auto flags = raw(__NR_fcntl, fd, F_GETFL);
    if (flags < 0) {
        return static_cast<int>(flags);
    }
    return static_cast<int>(raw(__NR_fcntl, fd, F_SETFL, flags | O_NONBLOCK));
}

auto poll(PollFd* fds, unsigned count, int timeout_ms) -> int
{
    timespec ts = { timeout_ms / 1000, static_cast<long>(timeout_ms % 1000) * 1'000'000 };
    auto* ts_ptr = timeout_ms < 0 ? nullptr : &ts;
    return static_cast<int>(raw(__NR_ppoll, arg(fds), count, arg(ts_ptr), 0, 8));
}

auto getenv(char const* name) -> char const*
{
    auto len = std::strlen(name);
    for (auto** e = environ; *e != nullptr; ++e) {
        if (std::strncmp(*e, name, len) == 0 && (*e)[len] == '=') {
            return *e + len + 1;
        }
    }
    return nullptr;
}

auto cpu_count() -> int
{
    unsigned long mask[16] = {};
    auto r = raw(__NR_sched_getaffinity, 0, sizeof(mask), arg(mask));
    if (r <= 0) {
        return 1;
    }
    auto count = 0;
    for (auto word : mask) {
        count += __builtin_popcountl(word);
    }
    return count > 0 ? count : 1;
}

auto page_size() -> std::size_t
{
    static auto cached = std::size_t { 0 };
    if (cached != 0) {
        return cached;
    }
    auto fd = open_ro("/proc/self/auxv");
    if (fd >= 0) {
        unsigned long pair[2] = {};
        while (read(fd, pair, sizeof(pair)) == sizeof(pair)) {
            if (pair[0] == AT_PAGESZ) {
                cached = pair[1];
                break;
            }
        }
        close(fd);
    }
    if (cached == 0) {
        cached = 4096;
    }
    return cached;
}

auto clock_monotonic_ns() -> std::int64_t
{
    timespec ts = {};
    raw(__NR_clock_gettime, CLOCK_MONOTONIC, arg(&ts));
    return std::int64_t { ts.tv_sec } * 1'000'000'000 + ts.tv_nsec;
}

auto clock_realtime_ns() -> std::int64_t
{
    timespec ts = {};
    raw(__NR_clock_gettime, CLOCK_REALTIME, arg(&ts));
    return std::int64_t { ts.tv_sec } * 1'000'000'000 + ts.tv_nsec;
}

auto isatty(int fd) -> bool
{
    char termios_buf[64] = {};
    return raw(__NR_ioctl, fd, TCGETS_REQUEST, arg(termios_buf)) == 0;
}

auto terminal_width(int fd) -> int
{
    winsize ws = {};
    if (raw(__NR_ioctl, fd, TIOCGWINSZ, arg(&ws)) != 0) {
        return 0;
    }
    return ws.ws_col;
}

} // namespace pup::platform::sys

#endif // __linux__ && __x86_64__
