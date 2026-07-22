// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/buf.hpp"
#include "pup/core/function.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace pup::platform {

struct FileStat {
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
};

[[nodiscard]]
auto stat_file(std::string_view path) -> Result<FileStat>;

class MappedFile final {
public:
    MappedFile();
    ~MappedFile();

    MappedFile(MappedFile const&) = delete;
    auto operator=(MappedFile const&) -> MappedFile& = delete;

    MappedFile(MappedFile&& other) noexcept;
    auto operator=(MappedFile&& other) noexcept -> MappedFile&;

    [[nodiscard]]
    static auto open(std::string_view path) -> Result<MappedFile>;

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
    std::string_view path,
    std::span<std::byte const> data
) -> Result<void>;

// Filesystem queries
[[nodiscard]]
auto exists(std::string_view path) -> bool;
[[nodiscard]]
auto is_file(std::string_view path) -> bool;
[[nodiscard]]
auto is_directory(std::string_view path) -> bool;
[[nodiscard]]
auto is_symlink(std::string_view path) -> bool;
[[nodiscard]]
auto is_empty(std::string_view path) -> bool;

// Filesystem mutations
[[nodiscard]]
auto create_directories(std::string_view path) -> Result<void>;
[[nodiscard]]
auto remove_file(std::string_view path) -> Result<void>;
[[nodiscard]]
auto remove_all(std::string_view path) -> Result<void>;
[[nodiscard]]
auto rename_path(std::string_view from, std::string_view to) -> Result<void>;
[[nodiscard]]
auto copy_file(std::string_view from, std::string_view to) -> Result<void>;

// Path resolution
[[nodiscard]]
auto current_directory() -> Result<StringId>;
[[nodiscard]]
auto canonical(std::string_view path) -> Result<StringId>;
[[nodiscard]]
auto absolute(std::string_view path) -> Result<StringId>;
[[nodiscard]]
auto read_symlink(std::string_view path) -> Result<StringId>;

// File I/O
[[nodiscard]]
auto read_file(std::string_view path, Buf& out) -> Result<void>;
[[nodiscard]]
auto write_file(std::string_view path, std::string_view data) -> Result<void>;

// Directory traversal.
//
// A DirEntry's `name` is a view into the `names` arena of the DirEntries that
// produced it — valid until the next read_directory on that DirEntries, or its
// destruction. In a walk, the `rel_path` handed to the visitor is likewise a
// view valid only for the duration of the call. Copy or intern to keep either.
struct DirEntry {
    std::string_view name;
    bool is_dir = false;
};
struct DirEntries {
    Buf names;
    Vec<DirEntry> entries;
};
[[nodiscard]]
auto read_directory(std::string_view path, DirEntries& out) -> Result<void>;

using WalkVisitor = Function<bool(DirEntry const&, std::string_view rel_path)>;
[[nodiscard]]
auto walk_directory(std::string_view path, WalkVisitor const& visitor) -> Result<void>;

} // namespace pup::platform
