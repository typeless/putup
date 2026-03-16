// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/file_io.hpp"
#include "pup/core/path.hpp"

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

auto MappedFile::open(std::string const& path) -> Result<MappedFile>
{
    auto file = MappedFile {};
    file.impl_ = std::make_unique<Impl>();

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    file.impl_->fd = ::open(path.c_str(), O_RDONLY);
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

auto stat_file(std::string const& path) -> Result<FileStat>
{
    struct stat st { };
    if (::stat(path.c_str(), &st) < 0) {
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
    std::string const& path,
    std::span<std::byte const> data
) -> Result<void>
{
    auto par = std::string { pup::path::parent(path) };
    if (!par.empty()) {
        auto r = create_directories(par);
        if (!r) {
            return r;
        }
    }

    auto temp_path = path + ".tmp.";

    struct timespec ts { };
    clock_gettime(CLOCK_MONOTONIC, &ts);
    auto seed = static_cast<unsigned>(getpid()) ^ static_cast<unsigned>(ts.tv_nsec);
    auto const* const hex = "0123456789abcdef";
    for (auto i = 0; i < 8; ++i) {
        seed = seed * 1103515245u + 12345u;
        temp_path += hex[(seed >> 16) & 0xF];
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

    if (::rename(temp_path.c_str(), path.c_str()) < 0) {
        ::unlink(temp_path.c_str());
        return make_error<void>(ErrorCode::IoError, "Failed to rename file");
    }

    return {};
}

// Filesystem queries

auto exists(std::string const& path) -> bool
{
    struct stat st { };
    return ::stat(path.c_str(), &st) == 0;
}

auto is_file(std::string const& path) -> bool
{
    struct stat st { };
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode);
}

auto is_directory(std::string const& path) -> bool
{
    struct stat st { };
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

auto is_symlink(std::string const& path) -> bool
{
    struct stat st { };
    if (::lstat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISLNK(st.st_mode);
}

auto is_empty(std::string const& path) -> bool
{
    struct stat st { };
    if (::stat(path.c_str(), &st) != 0) {
        return true;
    }
    if (S_ISDIR(st.st_mode)) {
        auto* dir = ::opendir(path.c_str());
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

auto mkdir_recursive(std::string const& path) -> bool
{
    if (path.empty()) {
        return true;
    }

    struct stat st { };
    if (::stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    auto par = std::string { pup::path::parent(path) };
    if (!par.empty() && par != path) {
        if (!mkdir_recursive(par)) {
            return false;
        }
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

} // namespace

auto create_directories(std::string const& path) -> Result<void>
{
    if (path.empty()) {
        return {};
    }
    if (!mkdir_recursive(path)) {
        return make_error<void>(ErrorCode::IoError, "Failed to create directories: " + path);
    }
    return {};
}

auto remove_file(std::string const& path) -> Result<void>
{
    struct stat st { };
    if (::lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            return {};
        }
        return make_error<void>(ErrorCode::IoError, "Failed to stat: " + path);
    }
    if (S_ISDIR(st.st_mode)) {
        if (::rmdir(path.c_str()) != 0) {
            return make_error<void>(ErrorCode::IoError, "Failed to remove directory: " + path);
        }
    } else {
        if (::unlink(path.c_str()) != 0) {
            return make_error<void>(ErrorCode::IoError, "Failed to remove file: " + path);
        }
    }
    return {};
}

namespace {

auto remove_all_recursive(std::string const& path) -> bool
{
    struct stat st { };
    if (::lstat(path.c_str(), &st) != 0) {
        return errno == ENOENT;
    }

    if (!S_ISDIR(st.st_mode)) {
        return ::unlink(path.c_str()) == 0;
    }

    auto* dir = ::opendir(path.c_str());
    if (!dir) {
        return false;
    }

    auto ok = true;
    struct dirent* entry = nullptr;
    while ((entry = ::readdir(dir)) != nullptr) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        auto child = path + "/" + entry->d_name;
        if (!remove_all_recursive(child)) {
            ok = false;
        }
    }
    ::closedir(dir);

    if (ok) {
        ok = (::rmdir(path.c_str()) == 0);
    }
    return ok;
}

} // namespace

auto remove_all(std::string const& path) -> Result<void>
{
    if (!remove_all_recursive(path)) {
        return make_error<void>(ErrorCode::IoError, "Failed to remove: " + path);
    }
    return {};
}

auto rename_path(std::string const& from, std::string const& to) -> Result<void>
{
    if (::rename(from.c_str(), to.c_str()) != 0) {
        return make_error<void>(ErrorCode::IoError, "Failed to rename: " + from + " -> " + to);
    }
    return {};
}

auto copy_file(std::string const& from, std::string const& to) -> Result<void>
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    auto src_fd = ::open(from.c_str(), O_RDONLY);
    if (src_fd < 0) {
        return make_error<void>(ErrorCode::IoError, "Failed to open source: " + from);
    }

    struct stat st { };
    ::fstat(src_fd, &st);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    auto dst_fd = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (dst_fd < 0) {
        ::close(src_fd);
        return make_error<void>(ErrorCode::IoError, "Failed to create destination: " + to);
    }

    char buf[8192];
    ssize_t n = 0;
    auto ok = true;
    while ((n = ::read(src_fd, buf, sizeof(buf))) > 0) {
        if (::write(dst_fd, buf, static_cast<std::size_t>(n)) != n) {
            ok = false;
            break;
        }
    }
    if (n < 0) {
        ok = false;
    }

    ::close(src_fd);
    ::close(dst_fd);

    if (!ok) {
        ::unlink(to.c_str());
        return make_error<void>(ErrorCode::IoError, "Failed to copy: " + from + " -> " + to);
    }
    return {};
}

// Path resolution

auto current_directory() -> Result<std::string>
{
    char buf[4096];
    if (::getcwd(buf, sizeof(buf)) == nullptr) {
        return make_error<std::string>(ErrorCode::IoError, "Failed to get current directory");
    }
    return std::string { buf };
}

auto canonical(std::string const& path) -> Result<std::string>
{
    char* resolved = ::realpath(path.c_str(), nullptr);
    if (resolved) {
        auto result = std::string { resolved };
        ::free(resolved); // NOLINT(cppcoreguidelines-no-malloc)
        return result;
    }

    auto abs = absolute(path);
    if (!abs) {
        return abs;
    }
    auto p = *abs;
    auto existing = p;
    auto tail = std::string {};
    while (!existing.empty()) {
        char* r = ::realpath(existing.c_str(), nullptr);
        if (r) {
            auto result = std::string { r };
            ::free(r); // NOLINT(cppcoreguidelines-no-malloc)
            if (!tail.empty()) {
                result = pup::path::join(result, pup::path::normalize(tail));
            }
            return result;
        }
        auto par = std::string { pup::path::parent(existing) };
        auto name = std::string { pup::path::filename(existing) };
        tail = tail.empty() ? name : pup::path::join(name, tail);
        if (par == existing) {
            break;
        }
        existing = par;
    }
    return pup::path::normalize(p);
}

auto absolute(std::string const& path) -> Result<std::string>
{
    if (!path.empty() && path[0] == '/') {
        return path;
    }
    auto cwd = current_directory();
    if (!cwd) {
        return cwd;
    }
    return pup::path::join(*cwd, path);
}

auto read_symlink(std::string const& path) -> Result<std::string>
{
    char buf[4096];
    auto n = ::readlink(path.c_str(), buf, sizeof(buf) - 1);
    if (n < 0) {
        return make_error<std::string>(ErrorCode::IoError, "Failed to read symlink: " + path);
    }
    buf[n] = '\0';
    return std::string { buf };
}

// File I/O

auto read_file(std::string const& path) -> Result<std::string>
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    auto fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return make_error<std::string>(ErrorCode::IoError, "Failed to open file: " + path);
    }

    struct stat st { };
    ::fstat(fd, &st);
    auto size = static_cast<std::size_t>(st.st_size);

    auto content = std::string(size, '\0');
    auto total = std::size_t { 0 };
    while (total < size) {
        auto n = ::read(fd, content.data() + total, size - total);
        if (n <= 0) {
            break;
        }
        total += static_cast<std::size_t>(n);
    }
    ::close(fd);

    content.resize(total);
    return content;
}

auto write_file(std::string const& path, std::string_view data) -> Result<void>
{
    auto par = std::string { pup::path::parent(path) };
    if (!par.empty()) {
        auto r = create_directories(par);
        if (!r) {
            return r;
        }
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    auto fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return make_error<void>(ErrorCode::IoError, "Failed to open file for writing: " + path);
    }

    auto n = ::write(fd, data.data(), data.size());
    ::close(fd);

    if (n != static_cast<ssize_t>(data.size())) {
        return make_error<void>(ErrorCode::IoError, "Failed to write file: " + path);
    }
    return {};
}

// Directory traversal

auto read_directory(std::string const& path) -> Result<std::vector<DirEntry>>
{
    auto* dir = ::opendir(path.c_str());
    if (!dir) {
        return make_error<std::vector<DirEntry>>(ErrorCode::IoError, "Failed to open directory: " + path);
    }

    auto entries = std::vector<DirEntry> {};
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
            auto full = path + "/" + entry->d_name;
            if (::stat(full.c_str(), &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
            }
#ifdef _DIRENT_HAVE_D_TYPE
        }
#endif
        entries.push_back(DirEntry { entry->d_name, is_dir });
    }
    ::closedir(dir);
    return entries;
}

namespace {

auto walk_recursive(
    std::string const& base,
    std::string const& rel,
    WalkVisitor const& visitor
) -> Result<void>
{
    auto full = rel.empty() ? base : base + "/" + rel;
    auto entries = read_directory(full);
    if (!entries) {
        return pup::unexpected<Error>(entries.error());
    }

    for (auto const& e : *entries) {
        auto child_rel = rel.empty() ? e.name : rel + "/" + e.name;
        auto should_recurse = visitor(e, child_rel);
        if (e.is_dir && should_recurse) {
            auto r = walk_recursive(base, child_rel, visitor);
            if (!r) {
                return r;
            }
        }
    }
    return {};
}

} // namespace

auto walk_directory(std::string const& path, WalkVisitor const& visitor) -> Result<void>
{
    return walk_recursive(path, "", visitor);
}

} // namespace pup::platform
