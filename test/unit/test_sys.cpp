// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#ifndef _WIN32

#    include "catch_amalgamated.hpp"
#    include "temp_root.hpp"

#    include "pup/platform/sys.hpp"

#    include <cerrno>
#    include <csignal>
#    include <cstdint>
#    include <cstdio>
#    include <cstdlib>
#    include <cstring>
#    include <ctime>
#    include <dirent.h>
#    include <fcntl.h>
#    include <filesystem>
#    include <poll.h>
#    include <set>
#    include <string>
#    include <string_view>
#    include <sys/stat.h>
#    include <sys/wait.h>
#    include <unistd.h>
#    include <vector>

#    ifdef __APPLE__
#        include <crt_externs.h>
#    else
extern char** environ; // NOLINT
#    endif

namespace sys = pup::platform::sys;

static_assert(sizeof(sys::PollFd) == sizeof(pollfd));
static_assert(sys::poll_in == POLLIN);
static_assert(sys::poll_hup == POLLHUP);
static_assert(sys::poll_err == POLLERR);
static_assert(sys::poll_nval == POLLNVAL);

namespace {

struct TempDir {
    std::string path;

    TempDir()
        : path { pup::test::temp_dir("pup_sys_test").string() }
    {
    }

    ~TempDir()
    {
        std::filesystem::remove_all(path);
    }

    auto sub(std::string const& name) const -> std::string
    {
        return path + "/" + name;
    }
};

struct CwdGuard {
    char saved[4096] = {};

    CwdGuard() { REQUIRE(::getcwd(saved, sizeof(saved)) != nullptr); }
    ~CwdGuard()
    {
        auto rc = ::chdir(saved);
        (void)rc;
    }
};

auto write_file(std::string const& path, std::string const& content) -> void
{
    auto* f = std::fopen(path.c_str(), "w");
    REQUIRE(f != nullptr);
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
}

auto libc_errno_of_open(char const* path) -> int
{
    errno = 0;
    auto fd = ::open(path, O_RDONLY);
    if (fd >= 0) {
        ::close(fd);
        return 0;
    }
    return errno;
}

auto test_environ() -> char* const*
{
#    ifdef __APPLE__
    return *_NSGetEnviron();
#    else
    return environ;
#    endif
}

auto ref_realpath(std::string const& path) -> std::string
{
    auto* r = ::realpath(path.c_str(), nullptr);
    if (r == nullptr) {
        return {};
    }
    auto out = std::string { r };
    ::free(r); // NOLINT(cppcoreguidelines-no-malloc)
    return out;
}

} // namespace

TEST_CASE("sys open/read/close match libc file content", "[sys]")
{
    auto t = TempDir {};
    write_file(t.sub("f.txt"), "hello sys layer");

    auto fd = sys::open_ro(t.sub("f.txt").c_str());
    REQUIRE(fd >= 0);
    char buf[64] = {};
    auto n = sys::read(fd, buf, sizeof(buf));
    REQUIRE(n == 15);
    REQUIRE(std::string_view { buf, 15 } == "hello sys layer");
    REQUIRE(sys::close(fd) == 0);

    auto missing = sys::open_ro(t.sub("absent").c_str());
    REQUIRE(missing == -libc_errno_of_open(t.sub("absent").c_str()));
    REQUIRE(missing == -ENOENT);
}

TEST_CASE("sys create_trunc/write/fstat roundtrip", "[sys]")
{
    auto t = TempDir {};
    auto path = t.sub("out.bin");

    auto fd = sys::create_trunc(path.c_str(), 0644);
    REQUIRE(fd >= 0);
    REQUIRE(sys::write(fd, "abcd", 4) == 4);
    REQUIRE(sys::fsync(fd) == 0);

    auto st = sys::Stat {};
    REQUIRE(sys::fstat(fd, st) == 0);
    REQUIRE(st.size == 4);
    REQUIRE(sys::is_reg(st.mode));
    REQUIRE(sys::close(fd) == 0);

    struct stat ref = {};
    REQUIRE(::stat(path.c_str(), &ref) == 0);
    REQUIRE((ref.st_mode & 0777) == (st.mode & 0777));
}

TEST_CASE("sys stat matches libc stat field-for-field", "[sys]")
{
    auto t = TempDir {};
    write_file(t.sub("f.txt"), "0123456789");

    auto st = sys::Stat {};
    REQUIRE(sys::stat(t.sub("f.txt").c_str(), st) == 0);

    struct stat ref = {};
    REQUIRE(::stat(t.sub("f.txt").c_str(), &ref) == 0);

    REQUIRE(st.mode == ref.st_mode);
    REQUIRE(st.size == static_cast<std::uint64_t>(ref.st_size));
#    ifdef __APPLE__
    auto ref_ns = std::int64_t { ref.st_mtimespec.tv_sec } * 1'000'000'000 + ref.st_mtimespec.tv_nsec;
#    else
    auto ref_ns = std::int64_t { ref.st_mtim.tv_sec } * 1'000'000'000 + ref.st_mtim.tv_nsec;
#    endif
    REQUIRE(st.mtime_ns == ref_ns);

    auto dst = sys::Stat {};
    REQUIRE(sys::stat(t.path.c_str(), dst) == 0);
    REQUIRE(sys::is_dir(dst.mode));

    REQUIRE(sys::stat(t.sub("absent").c_str(), st) == -ENOENT);
}

TEST_CASE("sys lstat sees the symlink itself", "[sys]")
{
    auto t = TempDir {};
    write_file(t.sub("target"), "x");
    REQUIRE(::symlink("target", t.sub("link").c_str()) == 0);

    auto st = sys::Stat {};
    REQUIRE(sys::lstat(t.sub("link").c_str(), st) == 0);
    REQUIRE(sys::is_lnk(st.mode));
    REQUIRE(sys::stat(t.sub("link").c_str(), st) == 0);
    REQUIRE(sys::is_reg(st.mode));
}

TEST_CASE("sys mkdir/rename/unlink/rmdir effects and errno parity", "[sys]")
{
    auto t = TempDir {};

    REQUIRE(sys::mkdir(t.sub("d").c_str(), 0755) == 0);
    auto st = sys::Stat {};
    REQUIRE(sys::stat(t.sub("d").c_str(), st) == 0);
    REQUIRE(sys::is_dir(st.mode));
    REQUIRE(sys::mkdir(t.sub("d").c_str(), 0755) == -EEXIST);

    write_file(t.sub("d/f"), "x");
    REQUIRE(sys::rmdir(t.sub("d").c_str()) == -ENOTEMPTY);

    REQUIRE(sys::rename(t.sub("d/f").c_str(), t.sub("g").c_str()) == 0);
    REQUIRE(sys::stat(t.sub("g").c_str(), st) == 0);
    REQUIRE(sys::rmdir(t.sub("d").c_str()) == 0);
    REQUIRE(sys::unlink(t.sub("g").c_str()) == 0);
    REQUIRE(sys::unlink(t.sub("g").c_str()) == -ENOENT);
}

TEST_CASE("sys getcwd/chdir match libc", "[sys]")
{
    auto guard = CwdGuard {};
    auto t = TempDir {};

    char ours[4096] = {};
    REQUIRE(sys::getcwd(ours, sizeof(ours)) > 0);
    REQUIRE(std::string { ours } == guard.saved);

    REQUIRE(sys::chdir(t.path.c_str()) == 0);
    REQUIRE(sys::getcwd(ours, sizeof(ours)) > 0);
    REQUIRE(std::string { ours } == ref_realpath(t.path));

    REQUIRE(sys::chdir(t.sub("absent").c_str()) == -ENOENT);
}

TEST_CASE("sys readlink returns target without nul", "[sys]")
{
    auto t = TempDir {};
    REQUIRE(::symlink("some/target", t.sub("l").c_str()) == 0);
    char buf[64] = {};
    auto n = sys::readlink(t.sub("l").c_str(), buf, sizeof(buf));
    REQUIRE(n == 11);
    REQUIRE(std::string_view { buf, 11 } == "some/target");
    REQUIRE(sys::readlink(t.sub("f").c_str(), buf, sizeof(buf)) == -ENOENT);
}

TEST_CASE("sys read_dir enumerates the same set as readdir", "[sys]")
{
    auto t = TempDir {};
    write_file(t.sub("a.txt"), "1");
    write_file(t.sub("b.txt"), "2");
    REQUIRE(sys::mkdir(t.sub("subdir").c_str(), 0755) == 0);
    REQUIRE(::symlink("a.txt", t.sub("lnk").c_str()) == 0);

    auto ref = std::set<std::string> {};
    auto* dp = ::opendir(t.path.c_str());
    REQUIRE(dp != nullptr);
    while (auto* e = ::readdir(dp)) {
        ref.insert(e->d_name);
    }
    ::closedir(dp);

    auto ours = std::set<std::string> {};
    auto types = std::set<std::string> {};
    auto d = sys::Dir {};
    REQUIRE(sys::open_dir(t.path.c_str(), d) == 0);
    auto entry = sys::DirEntry {};
    int rc = 0;
    while ((rc = sys::read_dir(d, entry)) == 1) {
        ours.insert(entry.name);
        if (entry.type == sys::FileType::Directory) {
            types.insert(std::string { entry.name } + "/d");
        }
        if (entry.type == sys::FileType::Symlink) {
            types.insert(std::string { entry.name } + "/l");
        }
    }
    REQUIRE(rc == 0);
    REQUIRE(sys::close_dir(d) == 0);

    REQUIRE(ours == ref);
    REQUIRE(types.count("subdir/d") == 1);
    REQUIRE(types.count("lnk/l") == 1);

    auto bad = sys::Dir {};
    REQUIRE(sys::open_dir(t.sub("absent").c_str(), bad) == -ENOENT);
}

TEST_CASE("sys pipe/dup2/nonblocking/poll", "[sys]")
{
    int fds[2] = { -1, -1 };
    REQUIRE(sys::pipe(fds) == 0);

    REQUIRE(sys::set_nonblocking(fds[0]) == 0);
    char c = 0;
    REQUIRE(sys::read(fds[0], &c, 1) == -EAGAIN);

    auto pf = sys::PollFd { .fd = fds[0], .events = sys::poll_in, .revents = 0 };
    REQUIRE(sys::poll(&pf, 1, 0) == 0);

    REQUIRE(sys::write(fds[1], "z", 1) == 1);
    REQUIRE(sys::poll(&pf, 1, 1000) == 1);
    REQUIRE((pf.revents & sys::poll_in) != 0);
    REQUIRE(sys::read(fds[0], &c, 1) == 1);
    REQUIRE(c == 'z');

    auto dup_target = 137;
    REQUIRE(sys::dup2(fds[1], dup_target) == dup_target);
    REQUIRE(sys::write(dup_target, "y", 1) == 1);
    REQUIRE(sys::read(fds[0], &c, 1) == 1);
    REQUIRE(c == 'y');
    REQUIRE(sys::dup2(dup_target, dup_target) == dup_target);

    REQUIRE(sys::close(fds[0]) == 0);
    REQUIRE(sys::close(fds[1]) == 0);
    REQUIRE(sys::close(dup_target) == 0);
}

TEST_CASE("sys fork/wait_pid decode exit and signal", "[sys]")
{
    auto pid = sys::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        _exit(42);
    }
    int status = 0;
    REQUIRE(sys::wait_pid(pid, status, false) == pid);
    REQUIRE(sys::exited(status));
    REQUIRE(!sys::signaled(status));
    REQUIRE(sys::exit_code(status) == 42);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 42);

    pid = sys::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        for (;;) {
            pause();
        }
    }
    REQUIRE(sys::kill(pid, SIGKILL) == 0);
    REQUIRE(sys::wait_pid(pid, status, false) == pid);
    REQUIRE(sys::signaled(status));
    REQUIRE(!sys::exited(status));
    REQUIRE(sys::term_signal(status) == SIGKILL);
    REQUIRE(WIFSIGNALED(status));
    REQUIRE(WTERMSIG(status) == SIGKILL);
}

TEST_CASE("sys execve runs a binary to completion", "[sys]")
{
    auto pid = sys::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        char sh[] = "/bin/sh";
        char dashc[] = "-c";
        char cmd[] = "exit 7";
        char* argv[] = { sh, dashc, cmd, nullptr };
        sys::execve("/bin/sh", argv, test_environ());
        _exit(127);
    }
    int status = 0;
    REQUIRE(sys::wait_pid(pid, status, false) == pid);
    REQUIRE(sys::exited(status));
    REQUIRE(sys::exit_code(status) == 7);
}

TEST_CASE("sys getenv matches libc", "[sys]")
{
    auto const* ours = sys::getenv("PATH");
    auto const* ref = ::getenv("PATH");
    REQUIRE((ours == nullptr) == (ref == nullptr));
    if (ref != nullptr) {
        REQUIRE(std::string { ours } == ref);
    }
    REQUIRE(sys::getenv("PUP_SYS_TEST_DEFINITELY_UNSET") == nullptr);
}

TEST_CASE("sys clocks agree with libc", "[sys]")
{
    auto a = sys::clock_monotonic_ns();
    auto b = sys::clock_monotonic_ns();
    REQUIRE(b >= a);

    timespec ts = {};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    auto ref = std::int64_t { ts.tv_sec } * 1'000'000'000 + ts.tv_nsec;
    auto ours = sys::clock_realtime_ns();
    REQUIRE(std::abs(ours - ref) < 1'000'000'000);
}

TEST_CASE("sys page_size and cpu_count are sane", "[sys]")
{
    REQUIRE(sys::page_size() == static_cast<std::size_t>(::sysconf(_SC_PAGESIZE)));
    auto n = sys::cpu_count();
    REQUIRE(n >= 1);
    REQUIRE(n <= ::sysconf(_SC_NPROCESSORS_ONLN));
}

TEST_CASE("sys isatty matches libc across std fds and a pipe", "[sys]")
{
    for (auto fd : { 0, 1, 2 }) {
        REQUIRE(sys::isatty(fd) == (::isatty(fd) != 0));
    }
    int fds[2] = { -1, -1 };
    REQUIRE(sys::pipe(fds) == 0);
    REQUIRE(!sys::isatty(fds[0]));
    sys::close(fds[0]);
    sys::close(fds[1]);
}

TEST_CASE("sys reserve/commit/unmap and file mapping", "[sys]")
{
    auto page = sys::page_size();
    auto* p = sys::reserve(4 * page);
    REQUIRE(p != nullptr);
    REQUIRE(sys::commit(p, page) == 0);
    static_cast<char*>(p)[0] = 'x';
    REQUIRE(static_cast<char*>(p)[0] == 'x');
    REQUIRE(sys::remap_none(p, page) == 0);
    REQUIRE(sys::commit(p, page) == 0);
    REQUIRE(static_cast<char*>(p)[0] == '\0');
    REQUIRE(sys::unmap(p, 4 * page) == 0);

    auto t = TempDir {};
    write_file(t.sub("m.bin"), "mapped-content");
    auto fd = sys::open_ro(t.sub("m.bin").c_str());
    REQUIRE(fd >= 0);
    auto* m = sys::map_file_ro(fd, 14);
    REQUIRE(m != nullptr);
    REQUIRE(std::string_view { static_cast<char const*>(m), 14 } == "mapped-content");
    REQUIRE(sys::unmap(m, 14) == 0);
    sys::close(fd);
}

TEST_CASE("sys realpath matches libc across link shapes", "[sys][realpath]")
{
    auto t = TempDir {};
    REQUIRE(sys::mkdir(t.sub("dir1").c_str(), 0755) == 0);
    REQUIRE(sys::mkdir(t.sub("dir1/dir2").c_str(), 0755) == 0);
    write_file(t.sub("dir1/f.txt"), "x");
    REQUIRE(::symlink((t.path + "/dir1").c_str(), t.sub("link_abs").c_str()) == 0);
    REQUIRE(::symlink("../f.txt", t.sub("dir1/dir2/link_rel").c_str()) == 0);
    REQUIRE(::symlink("link_abs", t.sub("l1").c_str()) == 0);

    auto cases = std::vector<std::string> {
        t.path,
        t.sub("dir1"),
        t.sub("dir1/f.txt"),
        t.sub("link_abs"),
        t.sub("link_abs/f.txt"),
        t.sub("dir1/dir2/link_rel"),
        t.sub("l1/f.txt"),
        t.sub("dir1/../dir1/./f.txt"),
        t.sub("dir1/dir2/../f.txt"),
        "/",
        "/tmp",
    };

    char out[sys::path_max] = {};
    for (auto const& c : cases) {
        auto ref = ref_realpath(c);
        auto n = sys::realpath(c.c_str(), out);
        CAPTURE(c);
        REQUIRE(n > 0);
        REQUIRE(std::string { out, static_cast<std::size_t>(n) } == ref);
    }
}

TEST_CASE("sys realpath error parity", "[sys][realpath]")
{
    auto t = TempDir {};
    char out[sys::path_max] = {};

    REQUIRE(sys::realpath(t.sub("absent").c_str(), out) == -ENOENT);

    REQUIRE(::symlink("dangling-target", t.sub("dangle").c_str()) == 0);
    REQUIRE(sys::realpath(t.sub("dangle").c_str(), out) == -ENOENT);

    REQUIRE(::symlink("loop_b", t.sub("loop_a").c_str()) == 0);
    REQUIRE(::symlink("loop_a", t.sub("loop_b").c_str()) == 0);
    REQUIRE(sys::realpath(t.sub("loop_a").c_str(), out) == -ELOOP);

    write_file(t.sub("plain"), "x");
    REQUIRE(sys::realpath(t.sub("plain/impossible").c_str(), out) == -ENOTDIR);
}

TEST_CASE("sys relative realpath resolves against cwd", "[sys][realpath]")
{
    auto guard = CwdGuard {};
    auto t = TempDir {};
    write_file(t.sub("f.txt"), "x");
    REQUIRE(sys::chdir(t.path.c_str()) == 0);

    char out[sys::path_max] = {};
    auto n = sys::realpath("f.txt", out);
    REQUIRE(n > 0);
    REQUIRE(std::string { out, static_cast<std::size_t>(n) } == ref_realpath("f.txt"));
}

#endif // _WIN32
