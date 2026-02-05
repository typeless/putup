// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/file_io.hpp"

#include <cstring>
#include <fcntl.h>
#include <random>
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

auto MappedFile::open(std::filesystem::path const& path) -> Result<MappedFile>
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

auto stat_file(std::filesystem::path const& path) -> Result<FileStat>
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

    auto temp_path = std::filesystem::path { path };
    temp_path += ".tmp.";

    auto rd = std::random_device {};
    auto gen = std::mt19937 { std::random_device::result_type { rd() } };
    auto dist = std::uniform_int_distribution<> { 0, 15 };
    auto const* const hex = "0123456789abcdef";
    for (auto i = 0; i < 8; ++i) {
        temp_path += hex[dist(gen)];
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    auto fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
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

} // namespace pup::platform
