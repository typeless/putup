// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/stable_vec.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/file_io.hpp"
#include "pup/platform/sys.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>

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

// The code is passed in, not read from errno: this binary is freestanding on Linux and has no errno.
auto append_reason(Buf& buf, int err) -> void
{
    if (err == 0) {
        return;
    }
    auto const* text = sys::error_text(err);
    buf.append(": ");
    if (text != nullptr) {
        buf.append(text);
        return;
    }
    buf.fmt("errno {}", err < 0 ? -err : err);
}

auto make_err_msg(std::string_view prefix, std::string_view path, int err) -> StringId
{
    auto buf = Buf {};
    buf.append(prefix);
    buf.append(path);
    append_reason(buf, err);
    return buf.intern(global_pool());
}

auto make_err_msg2(std::string_view prefix, std::string_view a, std::string_view mid, std::string_view b, int err) -> StringId
{
    auto buf = Buf {};
    buf.append(prefix);
    buf.append(a);
    buf.append(mid);
    buf.append(b);
    append_reason(buf, err);
    return buf.intern(global_pool());
}

// Retrying the remainder means the terminal failure carries the kernel's own code; a short
// write is not an error to guess at.
auto write_all(int fd, void const* data, std::size_t n) -> int
{
    auto const* p = static_cast<char const*>(data);
    auto off = std::size_t { 0 };
    while (off < n) {
        auto w = sys::write(fd, p + off, n - off);
        if (w < 0) {
            if (w == -EINTR) {
                continue;
            }
            return static_cast<int>(w);
        }
        if (w == 0) {
            return -EIO;
        }
        off += static_cast<std::size_t>(w);
    }
    return 0;
}

// Only these mean "not there". Any other stat failure means we cannot tell, and answering
// "absent" silently skips the caller's removal and orphans the file (#248).
auto stat_error_means_absent(int err) -> bool
{
    auto const e = err < 0 ? -err : err;
    return e == ENOENT || e == ENOTDIR || e == ENAMETOOLONG;
}

auto is_dot_or_dotdot(char const* name) -> bool
{
    return std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0;
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
    file.impl_->fd = sys::open_ro(p.c_str());
    if (file.impl_->fd < 0) {
        auto const err = file.impl_->fd;
        file.impl_.reset();
        return make_error<MappedFile>(ErrorCode::IoError, make_err_msg("Failed to open file: ", path, err));
    }

    auto st = sys::Stat {};
    if (auto rc = sys::fstat(file.impl_->fd, st); rc < 0) {
        sys::close(file.impl_->fd);
        file.impl_.reset();
        return make_error<MappedFile>(ErrorCode::IoError, make_err_msg("Failed to stat file: ", path, rc));
    }

    file.impl_->size = static_cast<std::size_t>(st.size);

    if (file.impl_->size > 0) {
        file.impl_->data = static_cast<std::byte*>(sys::map_file_ro(file.impl_->fd, file.impl_->size));
        if (file.impl_->data == nullptr) {
            sys::close(file.impl_->fd);
            file.impl_.reset();
            return make_error<MappedFile>(ErrorCode::IoError, make_err_msg("Failed to mmap file: ", path, 0));
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
        sys::unmap(impl_->data, impl_->size);
    }
    if (impl_->fd >= 0) {
        sys::close(impl_->fd);
    }
    impl_.reset();
}

auto stat_file(std::string_view path) -> Result<FileStat>
{
    auto p = CPath { path };
    auto st = sys::Stat {};
    if (auto rc = sys::stat(p.c_str(), st); rc != 0) {
        return make_error<FileStat>(ErrorCode::IoError, make_err_msg("Failed to stat file: ", path, rc));
    }

    return FileStat {
        .size = st.size,
        .mtime_ns = st.mtime_ns,
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

    auto seed = static_cast<unsigned>(sys::getpid()) ^ static_cast<unsigned>(sys::clock_monotonic_ns());
    auto const* const hex = "0123456789abcdef";
    for (auto i = 0; i < 8; ++i) {
        seed = seed * 1103515245u + 12345u;
        temp_path.append(hex[(seed >> 16) & 0xF]);
    }

    auto fd = sys::create_trunc(temp_path.c_str(), 0600);
    if (fd < 0) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to create temporary file: ", temp_path.view(), fd));
    }

    auto write_err = write_all(fd, data.data(), data.size());

    if (auto rc = sys::fsync(fd); rc < 0 && write_err == 0) {
        write_err = rc;
    }

    sys::close(fd);

    if (write_err != 0) {
        sys::unlink(temp_path.c_str());
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to write file: ", temp_path.view(), write_err));
    }

    auto p = CPath { path };
    if (auto rc = sys::rename(temp_path.c_str(), p.c_str()); rc != 0) {
        sys::unlink(temp_path.c_str());
        return make_error<void>(ErrorCode::IoError, make_err_msg2("Failed to rename: ", temp_path.view(), " -> ", path, rc));
    }

    return {};
}

// Filesystem queries

auto exists(std::string_view path) -> bool
{
    auto p = CPath { path };
    auto st = sys::Stat {};
    auto rc = sys::stat(p.c_str(), st);
    if (rc == 0) {
        return true;
    }
    return !stat_error_means_absent(rc);
}

auto is_file(std::string_view path) -> bool
{
    auto p = CPath { path };
    auto st = sys::Stat {};
    if (sys::stat(p.c_str(), st) != 0) {
        return false;
    }
    return sys::is_reg(st.mode);
}

auto is_directory(std::string_view path) -> bool
{
    auto p = CPath { path };
    auto st = sys::Stat {};
    if (sys::stat(p.c_str(), st) != 0) {
        return false;
    }
    return sys::is_dir(st.mode);
}

auto is_symlink(std::string_view path) -> bool
{
    auto p = CPath { path };
    auto st = sys::Stat {};
    if (sys::lstat(p.c_str(), st) != 0) {
        return false;
    }
    return sys::is_lnk(st.mode);
}

auto is_empty(std::string_view path) -> bool
{
    auto p = CPath { path };
    auto st = sys::Stat {};
    if (sys::stat(p.c_str(), st) != 0) {
        return true;
    }
    if (sys::is_dir(st.mode)) {
        auto d = sys::Dir {};
        if (sys::open_dir(p.c_str(), d) != 0) {
            return true;
        }
        auto entry = sys::DirEntry {};
        while (sys::read_dir(d, entry) == 1) {
            if (!is_dot_or_dotdot(entry.name)) {
                sys::close_dir(d);
                return false;
            }
        }
        sys::close_dir(d);
        return true;
    }
    return st.size == 0;
}

// Filesystem mutations

namespace {

auto mkdir_recursive(std::string_view path) -> int
{
    if (path.empty()) {
        return 0;
    }

    auto cp = CPath { path };
    auto st = sys::Stat {};
    if (sys::stat(cp.c_str(), st) == 0) {
        return sys::is_dir(st.mode) ? 0 : -ENOTDIR;
    }

    auto par = pup::path::parent(path);
    if (!par.empty() && par != path) {
        if (auto rc = mkdir_recursive(par); rc != 0) {
            return rc;
        }
    }

    auto rc = sys::mkdir(cp.c_str(), 0755);
    return (rc == 0 || rc == -EEXIST) ? 0 : rc;
}

} // namespace

auto create_directories(std::string_view path) -> Result<void>
{
    if (path.empty()) {
        return {};
    }
    if (auto rc = mkdir_recursive(path); rc != 0) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to create directories: ", path, rc));
    }
    return {};
}

auto remove_file(std::string_view path) -> Result<void>
{
    auto p = CPath { path };
    auto st = sys::Stat {};
    auto rc = sys::lstat(p.c_str(), st);
    if (rc != 0) {
        if (stat_error_means_absent(rc)) {
            return {};
        }
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to stat: ", path, rc));
    }
    if (sys::is_dir(st.mode)) {
        if (auto rm_rc = sys::rmdir(p.c_str()); rm_rc != 0) {
            return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to remove directory: ", path, rm_rc));
        }
    } else {
        if (auto rm_rc = sys::unlink(p.c_str()); rm_rc != 0) {
            return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to remove file: ", path, rm_rc));
        }
    }
    return {};
}

namespace {

auto remove_all_recursive(std::string_view path) -> int
{
    auto cp = CPath { path };
    auto st = sys::Stat {};
    auto rc = sys::lstat(cp.c_str(), st);
    if (rc != 0) {
        return stat_error_means_absent(rc) ? 0 : rc;
    }

    if (!sys::is_dir(st.mode)) {
        return sys::unlink(cp.c_str());
    }

    auto d = sys::Dir {};
    if (auto open_rc = sys::open_dir(cp.c_str(), d); open_rc != 0) {
        return open_rc;
    }

    // Collect first: unlinking while iterating the open directory can skip
    // entries under getdents batching.
    auto children = Vec<StringId> {};
    auto entry = sys::DirEntry {};
    while (sys::read_dir(d, entry) == 1) {
        if (!is_dot_or_dotdot(entry.name)) {
            children.push_back(pup::path::join(path, entry.name));
        }
    }
    sys::close_dir(d);

    // Every child is attempted; the first failure is the one reported.
    auto first_err = 0;
    for (auto child : children) {
        if (auto child_rc = remove_all_recursive(global_pool().get(child)); child_rc != 0 && first_err == 0) {
            first_err = child_rc;
        }
    }

    if (first_err != 0) {
        return first_err;
    }
    return sys::rmdir(cp.c_str());
}

} // namespace

auto remove_all(std::string_view path) -> Result<void>
{
    if (auto rc = remove_all_recursive(path); rc != 0) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to remove: ", path, rc));
    }
    return {};
}

auto rename_path(std::string_view from, std::string_view to) -> Result<void>
{
    auto f = CPath { from };
    auto t = CPath { to };
    if (auto rc = sys::rename(f.c_str(), t.c_str()); rc != 0) {
        return make_error<void>(ErrorCode::IoError, make_err_msg2("Failed to rename: ", from, " -> ", to, rc));
    }
    return {};
}

auto copy_file(std::string_view from, std::string_view to) -> Result<void>
{
    auto f = CPath { from };
    auto t = CPath { to };
    auto src_fd = sys::open_ro(f.c_str());
    if (src_fd < 0) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to open source: ", from, src_fd));
    }

    auto st = sys::Stat {};
    sys::fstat(src_fd, st);

    auto dst_fd = sys::create_trunc(t.c_str(), st.mode & 0777);
    if (dst_fd < 0) {
        sys::close(src_fd);
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to create destination: ", to, dst_fd));
    }

    char buf[8192];
    auto copy_err = 0;
    while (true) {
        auto n = sys::read(src_fd, buf, sizeof(buf));
        if (n < 0) {
            if (n == -EINTR) {
                continue;
            }
            copy_err = static_cast<int>(n);
            break;
        }
        if (n == 0) {
            break;
        }
        if (auto rc = write_all(dst_fd, buf, static_cast<std::size_t>(n)); rc != 0) {
            copy_err = rc;
            break;
        }
    }

    sys::close(src_fd);
    sys::close(dst_fd);

    if (copy_err != 0) {
        sys::unlink(t.c_str());
        return make_error<void>(ErrorCode::IoError, make_err_msg2("Failed to copy: ", from, " -> ", to, copy_err));
    }
    return {};
}

// Path resolution

auto current_directory() -> Result<StringId>
{
    char buf[4096];
    auto n = sys::getcwd(buf, sizeof(buf));
    if (n < 0) {
        return make_error<StringId>(ErrorCode::IoError, make_err_msg("Failed to get current directory", "", static_cast<int>(n)));
    }
    return global_pool().intern(std::string_view { buf, static_cast<std::size_t>(n) });
}

auto canonical(std::string_view path) -> Result<StringId>
{
    char resolved[sys::path_max];
    auto cp = CPath { path };
    auto n = sys::realpath(cp.c_str(), resolved);
    if (n > 0) {
        return global_pool().intern(std::string_view { resolved, static_cast<std::size_t>(n) });
    }

    auto& pool = pup::global_pool();
    auto abs_id = StringId {};
    if (!path.empty() && path[0] == '/') {
        abs_id = pool.intern(path);
    } else {
        auto cwd = current_directory();
        if (!cwd) {
            return cwd;
        }
        abs_id = pup::path::join(pool.get(*cwd), path);
    }
    auto p = pool.get(abs_id);
    auto existing_sv = p;
    auto tail_id = StringId::Empty;
    while (!existing_sv.empty()) {
        auto cp2 = CPath { existing_sv };
        auto rn = sys::realpath(cp2.c_str(), resolved);
        if (rn > 0) {
            // Normalizing after the join, not before: only here can a leading `..` cancel, and the prefix realpath returned is symlink-free.
            auto r_sv = std::string_view { resolved, static_cast<std::size_t>(rn) };
            return pup::path::normalize(pool.get(pup::path::join(r_sv, pool.get(tail_id))));
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

auto read_symlink(std::string_view path) -> Result<StringId>
{
    char buf[4096];
    auto p = CPath { path };
    auto n = sys::readlink(p.c_str(), buf, sizeof(buf) - 1);
    if (n < 0) {
        return make_error<StringId>(ErrorCode::IoError, make_err_msg("Failed to read symlink: ", path, static_cast<int>(n)));
    }
    return global_pool().intern(std::string_view { buf, static_cast<std::size_t>(n) });
}

// File I/O

auto read_file(std::string_view path, Buf& out) -> Result<void>
{
    auto p = CPath { path };
    auto fd = sys::open_ro(p.c_str());
    if (fd < 0) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to open file: ", path, fd));
    }

    auto st = sys::Stat {};
    sys::fstat(fd, st);
    auto size = static_cast<std::size_t>(st.size);

    out.clear();
    out.resize(size);
    auto* dest = out.data();
    auto total = std::size_t { 0 };
    while (total < size) {
        auto n = sys::read(fd, dest + total, size - total);
        if (n < 0) {
            if (n == -EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        total += static_cast<std::size_t>(n);
    }
    sys::close(fd);

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
    auto fd = sys::create_trunc(p.c_str(), 0644);
    if (fd < 0) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to open file for writing: ", path, fd));
    }

    auto rc = write_all(fd, data.data(), data.size());
    sys::close(fd);

    if (rc != 0) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to write file: ", path, rc));
    }
    return {};
}

// Directory traversal

auto read_directory(std::string_view path, DirEntries& out) -> Result<void>
{
    // Copy the path before clearing out: `path` may be a view into out.names
    // (e.g. an entry name from this same DirEntries), which clear() would wipe.
    auto p = CPath { path };
    out.names.clear();
    out.entries.clear();

    auto d = sys::Dir {};
    if (auto rc = sys::open_dir(p.c_str(), d); rc != 0) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to open directory: ", path, rc));
    }

    // Names concatenate into out.names, which may relocate as it grows, so views
    // are bound only after the fill completes. Offsets carry a trailing sentinel.
    auto offsets = Vec<std::uint32_t> {};
    auto stat_path = Buf {};
    auto entry = sys::DirEntry {};
    while (sys::read_dir(d, entry) == 1) {
        if (is_dot_or_dotdot(entry.name)) {
            continue;
        }
        auto is_dir = entry.type == sys::FileType::Directory;
        if (entry.type == sys::FileType::Unknown) {
            auto st = sys::Stat {};
            stat_path.clear();
            stat_path.append(path);
            if (!path.empty() && path.back() != '/') {
                stat_path.append('/');
            }
            stat_path.append(std::string_view { entry.name });
            if (sys::stat(stat_path.c_str(), st) == 0) {
                is_dir = sys::is_dir(st.mode);
            }
        }
        offsets.push_back(static_cast<std::uint32_t>(out.names.size()));
        out.names.append(std::string_view { entry.name });
        out.entries.push_back(DirEntry { {}, is_dir });
    }
    sys::close_dir(d);
    offsets.push_back(static_cast<std::uint32_t>(out.names.size()));

    auto const base = out.names.view();
    for (auto i = std::size_t { 0 }; i < out.entries.size(); ++i) {
        out.entries[i].name = base.substr(offsets[i], offsets[i + 1U] - offsets[i]);
    }
    std::ranges::sort(out.entries, {}, &DirEntry::name);
    return {};
}

namespace {

// One DirEntries per depth level, reused across sibling directories. A frame
// holding its own DirEntries (with Buf's 4 KB inline buffer) overflows the stack
// on deep trees; the pool keeps each frame small and bounds allocations to the
// max depth reached. Safe because the walk is strictly LIFO: only one directory
// per level is being iterated at any moment.
using ListingPool = StableVec<DirEntries>;

auto walk_recursive(
    Buf& path,
    std::size_t rel_start,
    std::size_t depth,
    ListingPool& pool,
    WalkVisitor const& visitor
) -> Result<void>
{
    while (depth >= pool.size()) {
        pool.emplace_back();
    }
    auto& listing = pool[depth];
    auto r = read_directory(path.view(), listing);
    if (!r) {
        return r;
    }

    auto const mark = path.size();
    for (auto const& e : listing.entries) {
        if (!path.view().empty() && path.view().back() != '/') {
            path.append('/');
        }
        path.append(e.name);
        auto should_recurse = visitor(e, path.view().substr(rel_start));
        if (e.is_dir && should_recurse) {
            auto rr = walk_recursive(path, rel_start, depth + 1, pool, visitor);
            if (!rr) {
                return rr;
            }
        }
        path.resize(mark);
    }
    return {};
}

} // namespace

auto walk_directory(std::string_view path, WalkVisitor const& visitor) -> Result<void>
{
    auto buf = Buf {};
    buf.append(path);
    auto const rel_start = (path.empty() || path.back() == '/') ? path.size() : path.size() + 1;
    auto pool = ListingPool {};
    return walk_recursive(buf, rel_start, 0, pool, visitor);
}

} // namespace pup::platform
