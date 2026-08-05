// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/stable_vec.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>

#include <windows.h>

namespace pup::platform {

namespace {

auto to_wide(std::string_view s) -> std::wstring
{
    if (s.empty()) {
        return {};
    }
    auto len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len == 0) {
        len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    }
    auto result = std::wstring(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), len);
    return result;
}

auto from_wide(std::wstring const& w, Buf& out) -> void
{
    out.clear();
    if (w.empty()) {
        return;
    }
    auto len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len == 0) {
        return;
    }
    out.resize(static_cast<std::size_t>(len));
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
}

auto backslash_to_forward(std::string_view sv, Buf& out) -> void
{
    out.clear();
    out.reserve(sv.size());
    for (auto c : sv) {
        out += (c == '\\') ? '/' : c;
    }
}

// GetLastError is read before anything else runs: these are called straight off a failed Win32 call.
auto append_reason(Buf& buf, DWORD err) -> void
{
    if (err == 0) {
        return;
    }
    wchar_t* text = nullptr;
    auto len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        err,
        0,
        reinterpret_cast<wchar_t*>(&text),
        0,
        nullptr
    );
    if (len == 0 || text == nullptr) {
        return;
    }
    while (len > 0 && (text[len - 1] == L'\n' || text[len - 1] == L'\r' || text[len - 1] == L' ')) {
        --len;
    }
    auto reason = Buf {};
    from_wide(std::wstring { text, len }, reason);
    LocalFree(text);
    if (reason.empty()) {
        return;
    }
    buf.append(": ");
    buf.append(std::string_view { reason.data(), reason.size() });
}

// Retrying the remainder means the terminal failure carries the system's own code; a short
// write is not an error to guess at.
auto write_all(HANDLE h, void const* data, std::size_t n) -> DWORD
{
    auto const* p = static_cast<char const*>(data);
    auto off = std::size_t { 0 };
    while (off < n) {
        auto chunk = static_cast<DWORD>(n - off);
        auto written = DWORD {};
        if (!WriteFile(h, p + off, chunk, &written, nullptr)) {
            return GetLastError();
        }
        if (written == 0) {
            return ERROR_WRITE_FAULT;
        }
        off += written;
    }
    return 0;
}

// Only these mean "not there". Any other attribute-query failure means we cannot tell, and
// answering "absent" silently skips the caller's removal and orphans the file (#248).
auto attr_error_means_absent(DWORD err) -> bool
{
    return err == ERROR_FILE_NOT_FOUND
        || err == ERROR_PATH_NOT_FOUND
        || err == ERROR_INVALID_NAME
        || err == ERROR_FILENAME_EXCED_RANGE;
}

auto make_err_msg(std::string_view prefix, std::string_view path, DWORD err) -> StringId
{
    auto buf = Buf {};
    buf.append(prefix);
    buf.append(path);
    append_reason(buf, err);
    return buf.intern(global_pool());
}

auto make_err_msg2(std::string_view prefix, std::string_view a, std::string_view mid, std::string_view b, DWORD err) -> StringId
{
    auto buf = Buf {};
    buf.append(prefix);
    buf.append(a);
    buf.append(mid);
    buf.append(b);
    append_reason(buf, err);
    return buf.intern(global_pool());
}

} // namespace

struct MappedFile::Impl {
    std::byte* data = nullptr;
    std::size_t size = 0;
    HANDLE file_handle = INVALID_HANDLE_VALUE;
    HANDLE mapping_handle = nullptr;
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
    auto wpath = to_wide(path);

    file.impl_->file_handle = CreateFileW(
        wpath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (file.impl_->file_handle == INVALID_HANDLE_VALUE) {
        auto const err = GetLastError();
        file.impl_.reset();
        return make_error<MappedFile>(ErrorCode::IoError, make_err_msg("Failed to open file: ", path, err));
    }

    auto file_size = LARGE_INTEGER {};
    if (!GetFileSizeEx(file.impl_->file_handle, &file_size)) {
        auto const err = GetLastError();
        CloseHandle(file.impl_->file_handle);
        file.impl_.reset();
        return make_error<MappedFile>(ErrorCode::IoError, make_err_msg("Failed to get file size: ", path, err));
    }

    file.impl_->size = static_cast<std::size_t>(file_size.QuadPart);

    if (file.impl_->size > 0) {
        file.impl_->mapping_handle = CreateFileMappingW(
            file.impl_->file_handle,
            nullptr,
            PAGE_READONLY,
            0,
            0,
            nullptr
        );

        if (!file.impl_->mapping_handle) {
            auto const err = GetLastError();
            CloseHandle(file.impl_->file_handle);
            file.impl_.reset();
            return make_error<MappedFile>(ErrorCode::IoError, make_err_msg("Failed to create file mapping: ", path, err));
        }

        file.impl_->data = static_cast<std::byte*>(
            MapViewOfFile(file.impl_->mapping_handle, FILE_MAP_READ, 0, 0, 0)
        );

        if (!file.impl_->data) {
            auto const err = GetLastError();
            CloseHandle(file.impl_->mapping_handle);
            CloseHandle(file.impl_->file_handle);
            file.impl_.reset();
            return make_error<MappedFile>(ErrorCode::IoError, make_err_msg("Failed to map view of file: ", path, err));
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
        UnmapViewOfFile(impl_->data);
    }
    if (impl_->mapping_handle) {
        CloseHandle(impl_->mapping_handle);
    }
    if (impl_->file_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->file_handle);
    }
    impl_.reset();
}

auto stat_file(std::string_view path) -> Result<FileStat>
{
    auto wpath = to_wide(path);
    auto attrs = WIN32_FILE_ATTRIBUTE_DATA {};
    if (!GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &attrs)) {
        return make_error<FileStat>(ErrorCode::IoError, make_err_msg("Failed to stat file: ", path, GetLastError()));
    }

    auto file_size = ULARGE_INTEGER {};
    file_size.LowPart = attrs.nFileSizeLow;
    file_size.HighPart = attrs.nFileSizeHigh;

    auto mtime = ULARGE_INTEGER {};
    mtime.LowPart = attrs.ftLastWriteTime.dwLowDateTime;
    mtime.HighPart = attrs.ftLastWriteTime.dwHighDateTime;
    auto constexpr UNIX_EPOCH_OFFSET = 116444736000000000ULL;
    auto mtime_ns = static_cast<std::int64_t>((mtime.QuadPart - UNIX_EPOCH_OFFSET) * 100);

    return FileStat {
        .size = file_size.QuadPart,
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

    auto seed = static_cast<unsigned>(GetCurrentProcessId()) ^ static_cast<unsigned>(GetTickCount());
    auto const* const hex = "0123456789abcdef";
    for (auto i = 0; i < 8; ++i) {
        seed = seed * 1103515245u + 12345u;
        temp_path += hex[(seed >> 16) & 0xF];
    }

    auto wtemp = to_wide(temp_path.view());
    auto file = CreateFileW(
        wtemp.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (file == INVALID_HANDLE_VALUE) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to create temporary file: ", temp_path.view(), GetLastError()));
    }

    auto write_err = write_all(file, data.data(), data.size());
    if (!FlushFileBuffers(file) && write_err == 0) {
        write_err = GetLastError();
    }
    CloseHandle(file);

    if (write_err != 0) {
        DeleteFileW(wtemp.c_str());
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to write file: ", temp_path.view(), write_err));
    }

    auto wpath = to_wide(path);
    if (!MoveFileExW(wtemp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        auto const err = GetLastError();
        DeleteFileW(wtemp.c_str());
        return make_error<void>(ErrorCode::IoError, make_err_msg2("Failed to rename: ", temp_path.view(), " -> ", path, err));
    }

    return {};
}

// Filesystem queries

auto exists(std::string_view path) -> bool
{
    auto wpath = to_wide(path);
    if (GetFileAttributesW(wpath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    return !attr_error_means_absent(GetLastError());
}

auto is_file(std::string_view path) -> bool
{
    auto wpath = to_wide(path);
    auto attrs = GetFileAttributesW(wpath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

auto is_directory(std::string_view path) -> bool
{
    auto wpath = to_wide(path);
    auto attrs = GetFileAttributesW(wpath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

auto is_symlink(std::string_view path) -> bool
{
    auto wpath = to_wide(path);
    auto attrs = GetFileAttributesW(wpath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

auto is_empty(std::string_view path) -> bool
{
    auto wpath = to_wide(path);
    auto attrs = GetFileAttributesW(wpath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        auto search = wpath + L"\\*";
        auto fd = WIN32_FIND_DATAW {};
        auto h = FindFirstFileW(search.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            return true;
        }
        auto empty = true;
        do {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                empty = false;
                break;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return empty;
    }
    auto file_data = WIN32_FILE_ATTRIBUTE_DATA {};
    if (!GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &file_data)) {
        return true;
    }
    return file_data.nFileSizeHigh == 0 && file_data.nFileSizeLow == 0;
}

// Filesystem mutations

auto create_directories(std::string_view path) -> Result<void>
{
    if (path.empty()) {
        return {};
    }
    auto par = pup::path::parent(path);
    if (!par.empty() && par != path) {
        auto r = create_directories(par);
        if (!r) {
            return r;
        }
    }
    auto wpath = to_wide(path);
    if (!CreateDirectoryW(wpath.c_str(), nullptr)) {
        auto err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to create directory: ", path, err));
        }
    }
    return {};
}

auto remove_file(std::string_view path) -> Result<void>
{
    auto wpath = to_wide(path);
    auto attrs = GetFileAttributesW(wpath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        auto const err = GetLastError();
        if (attr_error_means_absent(err)) {
            return {};
        }
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to query: ", path, err));
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        if (!RemoveDirectoryW(wpath.c_str())) {
            return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to remove directory: ", path, GetLastError()));
        }
    } else {
        if (!DeleteFileW(wpath.c_str())) {
            return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to remove file: ", path, GetLastError()));
        }
    }
    return {};
}

auto remove_all(std::string_view path) -> Result<void>
{
    auto wpath = to_wide(path);
    auto attrs = GetFileAttributesW(wpath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        auto const err = GetLastError();
        if (attr_error_means_absent(err)) {
            return {};
        }
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to query: ", path, err));
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!DeleteFileW(wpath.c_str())) {
            return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to remove file: ", path, GetLastError()));
        }
        return {};
    }
    auto listing = DirEntries {};
    if (read_directory(path, listing)) {
        for (auto const& e : listing.entries) {
            auto child_sv = global_pool().get(pup::path::join(path, e.name));
            auto r = remove_all(child_sv);
            if (!r) {
                return r;
            }
        }
    }
    if (!RemoveDirectoryW(wpath.c_str())) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to remove directory: ", path, GetLastError()));
    }
    return {};
}

auto rename_path(std::string_view from, std::string_view to) -> Result<void>
{
    auto wfrom = to_wide(from);
    auto wto = to_wide(to);
    if (!MoveFileExW(wfrom.c_str(), wto.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return make_error<void>(ErrorCode::IoError, make_err_msg2("Failed to rename: ", from, " -> ", to, GetLastError()));
    }
    return {};
}

auto copy_file(std::string_view from, std::string_view to) -> Result<void>
{
    auto wfrom = to_wide(from);
    auto wto = to_wide(to);
    if (!CopyFileW(wfrom.c_str(), wto.c_str(), FALSE)) {
        return make_error<void>(ErrorCode::IoError, make_err_msg2("Failed to copy: ", from, " -> ", to, GetLastError()));
    }
    return {};
}

// Path resolution

auto current_directory() -> Result<StringId>
{
    auto len = GetCurrentDirectoryW(0, nullptr);
    if (len == 0) {
        return make_error<StringId>(ErrorCode::IoError, make_err_msg("Failed to get current directory", "", GetLastError()));
    }
    auto wbuf = std::wstring(len, L'\0');
    auto written = GetCurrentDirectoryW(len, wbuf.data());
    if (written == 0 || written >= len) {
        return make_error<StringId>(ErrorCode::IoError, make_err_msg("Failed to get current directory", "", GetLastError()));
    }
    wbuf.resize(written);
    auto raw = Buf {};
    from_wide(wbuf, raw);
    auto fixed = Buf {};
    backslash_to_forward(raw.view(), fixed);
    return global_pool().intern(fixed.view());
}

auto canonical(std::string_view path) -> Result<StringId>
{
    auto wpath = to_wide(path);

    auto h = CreateFileW(
        wpath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr
    );

    if (h != INVALID_HANDLE_VALUE) {
        auto len = GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_NORMALIZED);
        auto wbuf = std::wstring(len, L'\0');
        auto written = len > 0 ? GetFinalPathNameByHandleW(h, wbuf.data(), len, FILE_NAME_NORMALIZED) : DWORD { 0 };
        CloseHandle(h);
        if (written > 0 && written < len) {
            wbuf.resize(written);
            auto raw = Buf {};
            from_wide(wbuf, raw);
            auto sv = raw.view();
            // Strip \\?\ prefix
            if (sv.size() > 4 && sv[0] == '\\' && sv[1] == '\\' && sv[2] == '?' && sv[3] == '\\') {
                sv = sv.substr(4);
            }
            auto fixed = Buf {};
            backslash_to_forward(sv, fixed);
            return global_pool().intern(fixed.view());
        }
    }

    // Fallback for non-existent paths: lexical resolution only
    auto len = GetFullPathNameW(wpath.c_str(), 0, nullptr, nullptr);
    if (len == 0) {
        return make_error<StringId>(ErrorCode::IoError, make_err_msg("Failed to resolve path: ", path, GetLastError()));
    }
    auto wbuf = std::wstring(len, L'\0');
    // The query call sizes the buffer before `..` collapsing; only the filling call reports what it actually wrote.
    auto written = GetFullPathNameW(wpath.c_str(), len, wbuf.data(), nullptr);
    if (written == 0 || written >= len) {
        return make_error<StringId>(ErrorCode::IoError, make_err_msg("Failed to resolve path: ", path, GetLastError()));
    }
    wbuf.resize(written);
    auto raw = Buf {};
    from_wide(wbuf, raw);
    auto fixed = Buf {};
    backslash_to_forward(raw.view(), fixed);
    return global_pool().intern(fixed.view());
}

auto absolute(std::string_view path) -> Result<StringId>
{
    return canonical(path);
}

auto read_symlink(std::string_view path) -> Result<StringId>
{
    auto wpath = to_wide(path);
    auto h = CreateFileW(
        wpath.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        return make_error<StringId>(ErrorCode::IoError, make_err_msg("Failed to read symlink: ", path, GetLastError()));
    }
    auto len = GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_NORMALIZED);
    if (len == 0) {
        auto const err = GetLastError();
        CloseHandle(h);
        return make_error<StringId>(ErrorCode::IoError, make_err_msg("Failed to read symlink: ", path, err));
    }
    auto wbuf = std::wstring(len, L'\0');
    auto written = GetFinalPathNameByHandleW(h, wbuf.data(), len, FILE_NAME_NORMALIZED);
    auto const fill_err = GetLastError();
    CloseHandle(h);
    if (written == 0 || written >= len) {
        return make_error<StringId>(ErrorCode::IoError, make_err_msg("Failed to read symlink: ", path, fill_err));
    }
    wbuf.resize(written);
    auto raw = Buf {};
    from_wide(wbuf, raw);
    auto fixed = Buf {};
    backslash_to_forward(raw.view(), fixed);
    return global_pool().intern(fixed.view());
}

// File I/O

auto read_file(std::string_view path, Buf& out) -> Result<void>
{
    auto wpath = to_wide(path);
    auto h = CreateFileW(
        wpath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to open file: ", path, GetLastError()));
    }
    auto file_size = LARGE_INTEGER {};
    if (!GetFileSizeEx(h, &file_size)) {
        auto const err = GetLastError();
        CloseHandle(h);
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to get file size: ", path, err));
    }
    auto size = static_cast<std::size_t>(file_size.QuadPart);
    out.clear();
    out.resize(size);
    auto* buf = out.data();
    auto total = std::size_t { 0 };
    while (total < size) {
        auto chunk = static_cast<DWORD>(std::min(size - total, std::size_t { 0x7FFF'FFFFu }));
        auto bytes_read = DWORD {};
        if (!ReadFile(h, buf + total, chunk, &bytes_read, nullptr) || bytes_read == 0) {
            break;
        }
        total += bytes_read;
    }
    CloseHandle(h);
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
    auto wpath = to_wide(path);
    auto h = CreateFileW(
        wpath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to open file for writing: ", path, GetLastError()));
    }
    auto err = write_all(h, data.data(), data.size());
    CloseHandle(h);
    if (err != 0) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to write file: ", path, err));
    }
    return {};
}

// Directory traversal

auto read_directory(std::string_view path, DirEntries& out) -> Result<void>
{
    // Convert the path before clearing out: `path` may be a view into out.names
    // (e.g. an entry name from this same DirEntries), which clear() would wipe.
    auto wpath = to_wide(path) + L"\\*";
    out.names.clear();
    out.entries.clear();

    auto fd = WIN32_FIND_DATAW {};
    auto h = FindFirstFileW(wpath.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to open directory: ", path, GetLastError()));
    }
    // Names concatenate into out.names, which may relocate as it grows, so views
    // are bound only after the fill completes. Offsets carry a trailing sentinel.
    auto offsets = Vec<std::uint32_t> {};
    auto name_buf = Buf {};
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        auto wname = std::wstring_view { fd.cFileName };
        auto wstr = std::wstring { wname };
        from_wide(wstr, name_buf);
        auto is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        offsets.push_back(static_cast<std::uint32_t>(out.names.size()));
        out.names.append(name_buf.view());
        out.entries.push_back(DirEntry { {}, is_dir });
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    offsets.push_back(static_cast<std::uint32_t>(out.names.size()));

    auto const base = out.names.view();
    for (auto i = std::size_t { 0 }; i < out.entries.size(); ++i) {
        out.entries[i].name = base.substr(offsets[i], offsets[i + 1] - offsets[i]);
    }
    std::ranges::sort(out.entries, {}, &DirEntry::name);
    return {};
}

auto walk_directory(std::string_view path, WalkVisitor const& visitor) -> Result<void>
{
    // One DirEntries per depth level, reused across sibling directories, so a deep
    // tree does not overflow the stack with per-frame Buf inline buffers. Safe
    // because the walk is strictly LIFO: one directory per level at a time.
    auto pool = StableVec<DirEntries> {};
    auto walk_impl = [&](auto& self, Buf& full, std::size_t rel_start, std::size_t depth) -> Result<void> {
        while (depth >= pool.size()) {
            pool.emplace_back();
        }
        auto& listing = pool[depth];
        auto r = read_directory(full.view(), listing);
        if (!r) {
            return r;
        }
        auto const mark = full.size();
        for (auto const& e : listing.entries) {
            if (!full.view().empty() && full.view().back() != '/') {
                full.append('/');
            }
            full.append(e.name);
            auto should_recurse = visitor(e, full.view().substr(rel_start));
            if (e.is_dir && should_recurse) {
                auto rr = self(self, full, rel_start, depth + 1);
                if (!rr) {
                    return rr;
                }
            }
            full.resize(mark);
        }
        return {};
    };
    auto buf = Buf {};
    buf.append(path);
    auto const rel_start = (path.empty() || path.back() == '/') ? path.size() : path.size() + 1;
    return walk_impl(walk_impl, buf, rel_start, 0);
}

} // namespace pup::platform
