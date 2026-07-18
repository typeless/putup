// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/file_io.hpp"

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
        file.impl_.reset();
        return make_error<MappedFile>(ErrorCode::IoError, "Failed to open file");
    }

    auto file_size = LARGE_INTEGER {};
    if (!GetFileSizeEx(file.impl_->file_handle, &file_size)) {
        CloseHandle(file.impl_->file_handle);
        file.impl_.reset();
        return make_error<MappedFile>(ErrorCode::IoError, "Failed to get file size");
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
            CloseHandle(file.impl_->file_handle);
            file.impl_.reset();
            return make_error<MappedFile>(ErrorCode::IoError, "Failed to create file mapping");
        }

        file.impl_->data = static_cast<std::byte*>(
            MapViewOfFile(file.impl_->mapping_handle, FILE_MAP_READ, 0, 0, 0)
        );

        if (!file.impl_->data) {
            CloseHandle(file.impl_->mapping_handle);
            CloseHandle(file.impl_->file_handle);
            file.impl_.reset();
            return make_error<MappedFile>(ErrorCode::IoError, "Failed to map view of file");
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
        return make_error<FileStat>(ErrorCode::IoError, "Failed to stat file");
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
        return make_error<void>(ErrorCode::IoError, "Failed to create temporary file");
    }

    auto bytes_written = DWORD {};
    auto write_ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &bytes_written, nullptr);
    auto flush_ok = FlushFileBuffers(file);
    CloseHandle(file);

    if (!write_ok || bytes_written != data.size() || !flush_ok) {
        DeleteFileW(wtemp.c_str());
        return make_error<void>(ErrorCode::IoError, "Failed to write file");
    }

    auto wpath = to_wide(path);
    if (!MoveFileExW(wtemp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(wtemp.c_str());
        return make_error<void>(ErrorCode::IoError, "Failed to rename file");
    }

    return {};
}

// Filesystem queries

auto exists(std::string_view path) -> bool
{
    auto wpath = to_wide(path);
    return GetFileAttributesW(wpath.c_str()) != INVALID_FILE_ATTRIBUTES;
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
            return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to create directory: ", path));
        }
    }
    return {};
}

auto remove_file(std::string_view path) -> Result<void>
{
    auto wpath = to_wide(path);
    auto attrs = GetFileAttributesW(wpath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return {};
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        if (!RemoveDirectoryW(wpath.c_str())) {
            return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to remove directory: ", path));
        }
    } else {
        if (!DeleteFileW(wpath.c_str())) {
            return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to remove file: ", path));
        }
    }
    return {};
}

auto remove_all(std::string_view path) -> Result<void>
{
    auto wpath = to_wide(path);
    auto attrs = GetFileAttributesW(wpath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return {};
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!DeleteFileW(wpath.c_str())) {
            return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to remove file: ", path));
        }
        return {};
    }
    auto entries = read_directory(path);
    if (entries) {
        for (auto const& e : *entries) {
            auto child_sv = global_pool().get(pup::path::join(path, global_pool().get(e.name)));
            auto r = remove_all(child_sv);
            if (!r) {
                return r;
            }
        }
    }
    if (!RemoveDirectoryW(wpath.c_str())) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to remove directory: ", path));
    }
    return {};
}

auto rename_path(std::string_view from, std::string_view to) -> Result<void>
{
    auto wfrom = to_wide(from);
    auto wto = to_wide(to);
    if (!MoveFileExW(wfrom.c_str(), wto.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return make_error<void>(ErrorCode::IoError, make_err_msg2("Failed to rename: ", from, " -> ", to));
    }
    return {};
}

auto copy_file(std::string_view from, std::string_view to) -> Result<void>
{
    auto wfrom = to_wide(from);
    auto wto = to_wide(to);
    if (!CopyFileW(wfrom.c_str(), wto.c_str(), FALSE)) {
        return make_error<void>(ErrorCode::IoError, make_err_msg2("Failed to copy: ", from, " -> ", to));
    }
    return {};
}

// Path resolution

auto current_directory() -> Result<StringId>
{
    auto len = GetCurrentDirectoryW(0, nullptr);
    if (len == 0) {
        return make_error<StringId>(ErrorCode::IoError, "Failed to get current directory");
    }
    auto wbuf = std::wstring(len, L'\0');
    GetCurrentDirectoryW(len, wbuf.data());
    wbuf.resize(len - 1);
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
        if (len > 0) {
            auto wbuf = std::wstring(len, L'\0');
            GetFinalPathNameByHandleW(h, wbuf.data(), len + 1, FILE_NAME_NORMALIZED);
            CloseHandle(h);
            wbuf.resize(len - 1);
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
        CloseHandle(h);
    }

    // Fallback for non-existent paths: lexical resolution only
    auto len = GetFullPathNameW(wpath.c_str(), 0, nullptr, nullptr);
    if (len == 0) {
        return make_error<StringId>(ErrorCode::IoError, make_err_msg("Failed to resolve path: ", path));
    }
    auto wbuf = std::wstring(len, L'\0');
    GetFullPathNameW(wpath.c_str(), len, wbuf.data(), nullptr);
    wbuf.resize(len - 1);
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
        return make_error<StringId>(ErrorCode::IoError, make_err_msg("Failed to read symlink: ", path));
    }
    auto len = GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_NORMALIZED);
    if (len == 0) {
        CloseHandle(h);
        return make_error<StringId>(ErrorCode::IoError, make_err_msg("Failed to read symlink: ", path));
    }
    auto wbuf = std::wstring(len, L'\0');
    GetFinalPathNameByHandleW(h, wbuf.data(), len + 1, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    wbuf.resize(len - 1);
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
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to open file: ", path));
    }
    auto file_size = LARGE_INTEGER {};
    if (!GetFileSizeEx(h, &file_size)) {
        CloseHandle(h);
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to get file size: ", path));
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
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to open file for writing: ", path));
    }
    auto bytes_written = DWORD {};
    auto ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &bytes_written, nullptr);
    CloseHandle(h);
    if (!ok || bytes_written != data.size()) {
        return make_error<void>(ErrorCode::IoError, make_err_msg("Failed to write file: ", path));
    }
    return {};
}

// Directory traversal

auto read_directory(std::string_view path) -> Result<Vec<DirEntry>>
{
    auto wpath = to_wide(path) + L"\\*";
    auto fd = WIN32_FIND_DATAW {};
    auto h = FindFirstFileW(wpath.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return make_error<Vec<DirEntry>>(ErrorCode::IoError, make_err_msg("Failed to open directory: ", path));
    }
    auto entries = Vec<DirEntry> {};
    auto name_buf = Buf {};
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        auto wname = std::wstring_view { fd.cFileName };
        auto wstr = std::wstring { wname };
        from_wide(wstr, name_buf);
        auto is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entries.push_back(DirEntry { global_pool().intern(name_buf.view()), is_dir });
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return entries;
}

auto walk_directory(std::string_view path, WalkVisitor const& visitor) -> Result<void>
{
    auto walk_impl = [&](auto& self, std::string_view base, std::string_view rel) -> Result<void> {
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
                auto r = self(self, base, child_rel_sv);
                if (!r) {
                    return r;
                }
            }
        }
        return {};
    };
    return walk_impl(walk_impl, path, std::string_view {});
}

} // namespace pup::platform
