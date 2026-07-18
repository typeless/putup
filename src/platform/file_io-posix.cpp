// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/file_io.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pup::platform {

namespace {

// Null-terminate a string_view for POSIX C APIs.
// Most paths fit in 256 bytes (Buf inline). Overflow falls back to heap.
struct CPath {
    Buf buf;

    explicit CPath(std::string_view sv)
    {
        buf.append(sv);
    }

    auto c_str() const -> char const* { return buf.c_str(); }
};

auto make_err_msg(std::string_view prefix, std::string_view path) -> StringId
{
    auto buf = Buf {};
    buf.append(prefix);
    buf.append(path);
    return buf.intern(global_pool());
}

auto make_err_msg2(std::string_view prefix, std::string_view a, std::string_view mid, std::string_view b) -> StringId
{
    auto buf = Buf {};
    buf.append(prefix);
    buf.append(a);
    buf.append(mid);
    buf.append(b);
    return buf.intern(global_pool());
}

} // namespace

struct MappedFile::Impl {
    std::byte* data = nullptr;
    std::size_t size = 0;
    int fd = -1;
};

MappedFile::MappedFile() = default;

MappedFile::~MappedFile()
{
    close();
}

MappedFile::MappedFile(MappedFile&& other) noexcept = default;

auto MappedFile::operator=(MappedFile&& other) noexcept -> MappedFile&
{
    if (this != &other) {
        close();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

auto MappedFile::data() const -> std::byte const*
{
    return impl_ ? impl_->data : nullptr;
}

auto MappedFile::size() const -> std::size_t
{
    return impl_ ? impl_->size : 0;
}

auto MappedFile::is_open() const -> bool
{
    return impl_ && impl_->data != nullptr;
}

auto MappedFile::open(std::string_view path) -> Result<MappedFile>
{
    auto file = MappedFile {};
    file.impl_ = std::make_unique<Impl>();

    auto p = CPath { path };
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    file.impl_->fd = ::open(p.c_str(), O_RDONLY);
    if (file.impl_->fd < 0) {
        file.impl_.reset();
        return make_error<MappedFile>(ErrorCode::IoError, "Failed to open file");
    }

    struct stat st { };
    if (::fstat(file.impl_->fd, &st) < 0) {
        ::close(file.impl_->fd);
        file.impl_.reset();
        return make_error<MappedFile>(ErrorCode::IoError, "Failed to stat file");
    }

    file.impl_->size = static_cast<std::size_t>(st.st_size);

    if (file.impl_->size > 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        file.impl_->data = reinterpret_cast<std::byte*>(
            ::mmap(nullptr, file.impl_->size, PROT_READ, MAP_PRIVATE, file.impl_->fd, 0)
        );
        if (file.impl_->data == MAP_FAILED) {
            file.impl_->data = nullptr;
            ::close(file.impl_->fd);
            file.impl_.reset();
            return make_error<MappedFile>(ErrorCode::IoError, "Failed to mmap file");
        }
    }

    return file;
}

auto MappedFile::close() -> void
{
    if (!impl_) {
        return;
    }

    if (impl_->data) {
        ::munmap(impl_->data, impl_->size);
    }
    if (impl_->fd >= 0) {
        ::close(impl_->fd);
    }
    impl_.reset();
}

auto stat_file(std::string_view path) -> Result<FileStat>
{
    auto p = CPath { path };
    struct stat st { };
    if (::stat(p.c_str(), &st) < 0) {
        return make_error<FileStat>(ErrorCode::IoError, "Failed to stat file");
    }

#ifdef __APPLE__
    auto mtime_ns = static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1'000'000'000LL
        + static_cast<std::int64_t>(st.st_mtimespec.tv_nsec);
#else
    auto mtime_ns = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000LL
        + static_cast<std::int64_t>(st.st_mtim.tv_nsec);
#endif

    return FileStat {
        .size = static_cast<std::uint64_t>(st.st_size),
        .mtime_ns = mtime_ns,
    };
}

auto atomic_write(
    std::string_view path,
    std::span<std::byte const> data
) -> Result<void>
{
    auto par = pup::path::parent(path);
    if (!par.empty()) {
        auto r = create_directories(par);
        if (!r) {
            return r;
        }
    }

    auto temp_path = Buf {};
    temp_path.append(path);
    temp_path.append(".tmp.");

    struct timespec ts { };
    clock_gettime(CLOCK_MONOTONIC, &ts);
    auto seed = static_cast<unsigned>(getpid()) ^ static_cast<unsigned>(ts.tv_nsec);
    auto const* const hex = "0123456789abcdef";
    for (auto i = 0; i < 8; ++i) {
        seed = seed * 1103515245u + 12345u;
        temp_path.append(hex[(seed >> 16) & 0xF]);
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    auto fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return make_error<void>(ErrorCode::IoError, "Failed to create temporary file");
    }

    auto bytes_written = ::write(fd, data.data(), data.size());
    auto write_error = (bytes_written != static_cast<ssize_t>(data.size()));

    if (::fsync(fd) < 0) {
        write_error = true;
    }

    ::close(fd);

    if (write_error) {
        ::unlink(temp_path.c_str());
        return make_error<void>(ErrorCode::IoError, "Failed to write file");
    }

    auto p = CPath { path };
    if (::rename(temp_path.c_str(), p.c_str()) < 0) {
        ::unlink(temp_path.c_str());
        return make_error<void>(ErrorCode::IoError, "Failed to rename file");
    }

    return {};
}

// Filesystem queries

auto exists(std::string_view path) -> bool
{
    auto p = CPath { path };
    struct stat st { };
    return ::stat(p.c_str(), &st) == 0;
}

auto is_file(std::string_view path) -> bool
{
    auto p = CPath { path };
    struct stat st { };
    if (::stat(p.c_str(), &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode);
}

auto is_directory(std::string_view path) -> bool
{
    auto p = CPath { path };
    struct stat st { };
    if (::stat(p.c_str(), &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

auto is_symlink(std::string_view path) -> bool
{
    auto p = CPath { path };
    struct stat st { };
    if (::lstat(p.c_str(), &st) != 0) {
        return false;
    }
    return S_ISLNK(st.st_mode);
}

auto is_empty(std::string_view path) -> bool
{
    auto p = CPath { path };
    struct stat st { };
    if (::stat(p.c_str(), &st) != 0) {
        return true;
    }
    if (S_ISDIR(st.st_mode)) {
        auto* dir = ::opendir(p.c_str());
        if (!dir) {
            return true;
        }
        struct dirent* entry = nullptr;
        while ((entry = ::readdir(dir)) != nullptr) {
            if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) {
                ::closedir(dir);
                return false;
            }
        }
        ::closedir(dir);
        return true;
    }
    return st.st_size == 0;
}

// Filesystem mutations

namespace {

auto mkdir_recursive(std::string_view path) -> bool
{
    if (path.empty()) {
        return true;
    }

    auto cp = CPath { path };
    struct stat st { };
    if (::stat(cp.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    auto par = pup::path::parent(path);
    if (!par.empty() && par != path) {
        if (!mkdir_recursive(par)) {
            return false;
        }
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return ::mkdir(cp.c_str(), 0755) == 0 || errno == EEXIST;
}

} // namespace

auto create_directories(std::string_view path) -> Result<void>
{
    if (path.empty()) {
        return {};
    }
    if (!mkdir_recursive(path)) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to create directories: ", path));
    }
    return {};
}

auto remove_file(std::string_view path) -> Result<void>
{
    auto p = CPath { path };
    struct stat st { };
    if (::lstat(p.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            return {};
        }
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to stat: ", path));
    }
    if (S_ISDIR(st.st_mode)) {
        if (::rmdir(p.c_str()) != 0) {
            return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to remove directory: ", path));
        }
    } else {
        if (::unlink(p.c_str()) != 0) {
            return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to remove file: ", path));
        }
    }
    return {};
}

namespace {

auto remove_all_recursive(std::string_view path) -> bool
{
    auto cp = CPath { path };
    struct stat st { };
    if (::lstat(cp.c_str(), &st) != 0) {
        return errno == ENOENT;
    }

    if (!S_ISDIR(st.st_mode)) {
        return ::unlink(cp.c_str()) == 0;
    }

    auto* dir = ::opendir(cp.c_str());
    if (!dir) {
        return false;
    }

    auto ok = true;
    struct dirent* entry = nullptr;
    while ((entry = ::readdir(dir)) != nullptr) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        auto child_sv = global_pool().get(pup::path::join(path, entry->d_name));
        if (!remove_all_recursive(child_sv)) {
            ok = false;
        }
    }
    ::closedir(dir);

    if (ok) {
        ok = (::rmdir(cp.c_str()) == 0);
    }
    return ok;
}

} // namespace

auto remove_all(std::string_view path) -> Result<void>
{
    if (!remove_all_recursive(path)) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to remove: ", path));
    }
    return {};
}

auto rename_path(std::string_view from, std::string_view to) -> Result<void>
{
    auto f = CPath { from };
    auto t = CPath { to };
    if (::rename(f.c_str(), t.c_str()) != 0) {
        return make_error<void>(ErrorCode::IoError, make_err_msg2("Failed to rename: ", from, " -> ", to));
    }
    return {};
}

auto copy_file(std::string_view from, std::string_view to) -> Result<void>
{
    auto f = CPath { from };
    auto t = CPath { to };
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    auto src_fd = ::open(f.c_str(), O_RDONLY);
    if (src_fd < 0) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to open source: ", from));
    }

    struct stat st { };
    ::fstat(src_fd, &st);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    auto dst_fd = ::open(t.c_str(), O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (dst_fd < 0) {
        ::close(src_fd);
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to create destination: ", to));
    }

    char buf[8192];
    auto ok = true;
    while (true) {
        auto n = ::read(src_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        if (n == 0) {
            break;
        }
        if (::write(dst_fd, buf, static_cast<std::size_t>(n)) != n) {
            ok = false;
            break;
        }
    }

    ::close(src_fd);
    ::close(dst_fd);

    if (!ok) {
        ::unlink(t.c_str());
        return make_error<void>(ErrorCode::IoError, make_err_msg2("Failed to copy: ", from, " -> ", to));
    }
    return {};
}

// Path resolution

auto current_directory() -> Result<StringId>
{
    char buf[4096];
    if (::getcwd(buf, sizeof(buf)) == nullptr) {
        return make_error<StringId>(ErrorCode::IoError, "Failed to get current directory");
    }
    return global_pool().intern(buf);
}

auto canonical(std::string_view path) -> Result<StringId>
{
    auto cp = CPath { path };
    char* resolved = ::realpath(cp.c_str(), nullptr);
    if (resolved) {
        auto result = global_pool().intern(resolved);
        ::free(resolved); // NOLINT(cppcoreguidelines-no-malloc)
        return result;
    }

    auto abs = absolute(path);
    if (!abs) {
        return abs;
    }
    auto& pool = pup::global_pool();
    auto p = pool.get(*abs);
    auto existing_sv = p;
    auto tail_id = StringId::Empty;
    while (!existing_sv.empty()) {
        auto cp2 = CPath { existing_sv };
        char* r = ::realpath(cp2.c_str(), nullptr);
        if (r) {
            auto r_sv = std::string_view { r };
            StringId result;
            if (!pup::is_empty(tail_id)) {
                result = pup::path::join(r_sv, pool.get(pup::path::normalize(pool.get(tail_id))));
            } else {
                result = pool.intern(r_sv);
            }
            ::free(r); // NOLINT(cppcoreguidelines-no-malloc)
            return result;
        }
        auto par = pup::path::parent(existing_sv);
        auto name = pup::path::filename(existing_sv);
        tail_id = pup::is_empty(tail_id) ? pool.intern(name) : pup::path::join(name, pool.get(tail_id));
        if (par == existing_sv) {
            break;
        }
        existing_sv = par;
    }
    return pup::path::normalize(p);
}

auto absolute(std::string_view path) -> Result<StringId>
{
    if (!path.empty() && path[0] == '/') {
        return global_pool().intern(path);
    }
    auto cwd = current_directory();
    if (!cwd) {
        return cwd;
    }
    return pup::path::join(global_pool().get(*cwd), path);
}

auto read_symlink(std::string_view path) -> Result<StringId>
{
    char buf[4096];
    auto p = CPath { path };
    auto n = ::readlink(p.c_str(), buf, sizeof(buf) - 1);
    if (n < 0) {
        return make_error<StringId>(ErrorCode::IoError, make_err_msg("Failed to read symlink: ", path));
    }
    buf[n] = '\0';
    return global_pool().intern(std::string_view { buf, static_cast<std::size_t>(n) });
}

// File I/O

auto read_file(std::string_view path, Buf& out) -> Result<void>
{
    auto p = CPath { path };
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    auto fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to open file: ", path));
    }

    struct stat st { };
    ::fstat(fd, &st);
    auto size = static_cast<std::size_t>(st.st_size);

    out.clear();
    out.resize(size);
    auto* dest = out.data();
    auto total = std::size_t { 0 };
    while (total < size) {
        auto n = ::read(fd, dest + total, size - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        total += static_cast<std::size_t>(n);
    }
    ::close(fd);

    out.resize(total);
    return {};
}

auto write_file(std::string_view path, std::string_view data) -> Result<void>
{
    auto par = pup::path::parent(path);
    if (!par.empty()) {
        auto r = create_directories(par);
        if (!r) {
            return r;
        }
    }

    auto p = CPath { path };
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    auto fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to open file for writing: ", path));
    }

    auto n = ::write(fd, data.data(), data.size());
    ::close(fd);

    if (n != static_cast<ssize_t>(data.size())) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to write file: ", path));
    }
    return {};
}

// Directory traversal

auto read_directory(std::string_view path) -> Result<Vec<DirEntry>>
{
    auto p = CPath { path };
    auto* dir = ::opendir(p.c_str());
    if (!dir) {
        return make_error<Vec<DirEntry>>(ErrorCode::IoError, make_err_msg("Failed to open directory: ", path));
    }

    auto entries = Vec<DirEntry> {};
    struct dirent* entry = nullptr;
    while ((entry = ::readdir(dir)) != nullptr) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        auto is_dir = false;
#ifdef _DIRENT_HAVE_D_TYPE
        if (entry->d_type == DT_DIR) {
            is_dir = true;
        } else if (entry->d_type == DT_UNKNOWN) {
#endif
            struct stat st { };
            auto full_sv = global_pool().get(pup::path::join(path, entry->d_name));
            auto full_cp = CPath { full_sv };
            if (::stat(full_cp.c_str(), &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
            }
#ifdef _DIRENT_HAVE_D_TYPE
        }
#endif
        entries.push_back(DirEntry { global_pool().intern(entry->d_name), is_dir });
    }
    ::closedir(dir);
    return entries;
}

namespace {

auto walk_recursive(
    std::string_view base,
    std::string_view rel,
    WalkVisitor const& visitor
) -> Result<void>
{
    auto full_sv = rel.empty() ? base : global_pool().get(pup::path::join(base, rel));
    auto entries = read_directory(full_sv);
    if (!entries) {
        return pup::unexpected<Error>(entries.error());
    }

    for (auto const& e : *entries) {
        auto name_sv = global_pool().get(e.name);
        auto child_rel_sv = rel.empty()
            ? name_sv
            : global_pool().get(pup::path::join(rel, name_sv));
        auto should_recurse = visitor(e, child_rel_sv);
        if (e.is_dir && should_recurse) {
            auto r = walk_recursive(base, child_rel_sv, visitor);
            if (!r) {
                return r;
            }
        }
    }
    return {};
}

} // namespace

auto walk_directory(std::string_view path, WalkVisitor const& visitor) -> Result<void>
{
    return walk_recursive(path, "", visitor);
}

} // namespace pup::platform
