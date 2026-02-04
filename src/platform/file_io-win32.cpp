// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/file_io.hpp"

#include <windows.h>

namespace pup::platform {

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

auto MappedFile::open(std::filesystem::path const& path) -> Result<MappedFile>
{
    auto file = MappedFile {};
    file.impl_ = std::make_unique<Impl>();

    file.impl_->file_handle = CreateFileW(
        path.c_str(),
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

auto stat_file(std::filesystem::path const& path) -> Result<FileStat>
{
    auto attrs = WIN32_FILE_ATTRIBUTE_DATA {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrs)) {
        return make_error<FileStat>(ErrorCode::IoError, "Failed to stat file");
    }

    auto file_size = ULARGE_INTEGER {};
    file_size.LowPart = attrs.nFileSizeLow;
    file_size.HighPart = attrs.nFileSizeHigh;

    // Convert FILETIME to nanoseconds since Unix epoch
    // FILETIME is 100-nanosecond intervals since Jan 1, 1601
    // Unix epoch is Jan 1, 1970 - difference is 116444736000000000 100-ns intervals
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
    std::filesystem::path const& path,
    std::span<std::byte const> data
) -> Result<void>
{
    auto parent = path.parent_path();
    if (!parent.empty()) {
        auto ec = std::error_code {};
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return make_error<void>(ErrorCode::IoError, "Failed to create directory");
        }
    }

    auto temp_path = path;
    temp_path += L".tmp.";

    WCHAR temp_suffix[16];
    for (int i = 0; i < 8; ++i) {
        temp_suffix[i] = L"0123456789abcdef"[rand() % 16];
    }
    temp_suffix[8] = L'\0';
    temp_path += temp_suffix;

    auto file = CreateFileW(
        temp_path.c_str(),
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
        DeleteFileW(temp_path.c_str());
        return make_error<void>(ErrorCode::IoError, "Failed to write file");
    }

    if (!MoveFileExW(temp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp_path.c_str());
        return make_error<void>(ErrorCode::IoError, "Failed to rename file");
    }

    return {};
}

} // namespace pup::platform
