// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/result.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pup::platform {

struct FileStat {
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
};

[[nodiscard]]
auto stat_file(std::string const& path) -> Result<FileStat>;

class MappedFile {
public:
    MappedFile();
    ~MappedFile();

    MappedFile(MappedFile const&) = delete;
    auto operator=(MappedFile const&) -> MappedFile& = delete;

    MappedFile(MappedFile&& other) noexcept;
    auto operator=(MappedFile&& other) noexcept -> MappedFile&;

    [[nodiscard]]
    static auto open(std::string const& path) -> Result<MappedFile>;

    [[nodiscard]]
    auto data() const -> std::byte const*;

    [[nodiscard]]
    auto size() const -> std::size_t;

    [[nodiscard]]
    auto is_open() const -> bool;

    auto close() -> void;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]]
auto atomic_write(
    std::string const& path,
    std::span<std::byte const> data
) -> Result<void>;

// Filesystem queries
[[nodiscard]] auto exists(std::string const& path) -> bool;
[[nodiscard]] auto is_file(std::string const& path) -> bool;
[[nodiscard]] auto is_directory(std::string const& path) -> bool;
[[nodiscard]] auto is_symlink(std::string const& path) -> bool;
[[nodiscard]] auto is_empty(std::string const& path) -> bool;

// Filesystem mutations
[[nodiscard]] auto create_directories(std::string const& path) -> Result<void>;
[[nodiscard]] auto remove_file(std::string const& path) -> Result<void>;
[[nodiscard]] auto remove_all(std::string const& path) -> Result<void>;
[[nodiscard]] auto rename_path(std::string const& from, std::string const& to) -> Result<void>;
[[nodiscard]] auto copy_file(std::string const& from, std::string const& to) -> Result<void>;

// Path resolution
[[nodiscard]] auto current_directory() -> Result<std::string>;
[[nodiscard]] auto canonical(std::string const& path) -> Result<std::string>;
[[nodiscard]] auto absolute(std::string const& path) -> Result<std::string>;
[[nodiscard]] auto read_symlink(std::string const& path) -> Result<std::string>;

// File I/O
[[nodiscard]] auto read_file(std::string const& path) -> Result<std::string>;
[[nodiscard]] auto write_file(std::string const& path, std::string_view data) -> Result<void>;

// Directory traversal
struct DirEntry {
    std::string name;
    bool is_dir = false;
};
[[nodiscard]] auto read_directory(std::string const& path) -> Result<std::vector<DirEntry>>;

using WalkVisitor = std::function<bool(DirEntry const&, std::string const& rel_path)>;
[[nodiscard]] auto walk_directory(std::string const& path, WalkVisitor const& visitor) -> Result<void>;

} // namespace pup::platform
