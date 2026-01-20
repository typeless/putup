// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace pup::platform {

/// File metadata from stat
struct FileStat {
    std::uint64_t size = 0;
};

/// Get file metadata
[[nodiscard]]
auto stat_file(std::filesystem::path const& path) -> Result<FileStat>;

/// Memory-mapped read-only file (platform-specific data via pimpl)
class MappedFile {
public:
    MappedFile();
    ~MappedFile();

    MappedFile(MappedFile const&) = delete;
    auto operator=(MappedFile const&) -> MappedFile& = delete;

    MappedFile(MappedFile&& other) noexcept;
    auto operator=(MappedFile&& other) noexcept -> MappedFile&;

    /// Open a file for memory-mapped reading
    [[nodiscard]]
    static auto open(std::filesystem::path const& path) -> Result<MappedFile>;

    /// Get pointer to mapped data
    [[nodiscard]]
    auto data() const -> std::byte const*;

    /// Get size of mapped data
    [[nodiscard]]
    auto size() const -> std::size_t;

    /// Check if file is open
    [[nodiscard]]
    auto is_open() const -> bool;

    /// Close the mapped file
    auto close() -> void;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Write data atomically to a file (write to temp, then rename)
[[nodiscard]]
auto atomic_write(
    std::filesystem::path const& path,
    std::span<std::byte const> data
) -> Result<void>;

} // namespace pup::platform
