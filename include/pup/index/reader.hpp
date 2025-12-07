// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "entry.hpp"
#include "format.hpp"
#include "pup/core/result.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>

namespace pup::index {

/// Memory-mapped index file reader
class IndexReader {
public:
    IndexReader() = default;
    ~IndexReader();

    IndexReader(IndexReader const&) = delete;
    auto operator=(IndexReader const&) -> IndexReader& = delete;

    IndexReader(IndexReader&& other) noexcept;
    auto operator=(IndexReader&& other) noexcept -> IndexReader&;

    /// Open an index file for reading
    [[nodiscard]] static auto open(std::filesystem::path const& path) -> Result<IndexReader>;

    /// Check if a file is a valid index file (checks magic and version)
    [[nodiscard]] static auto is_valid_index(std::filesystem::path const& path) -> bool;

    /// Read the entire index into memory
    [[nodiscard]] auto read() const -> Result<Index>;

    /// Get the header
    [[nodiscard]] auto header() const -> RawHeader const*;

    /// Get file entries as raw span
    [[nodiscard]] auto raw_files() const -> std::span<RawFileEntry const>;

    /// Get command entries as raw span
    [[nodiscard]] auto raw_commands() const -> std::span<RawCommandEntry const>;

    /// Get edge entries as raw span
    [[nodiscard]] auto raw_edges() const -> std::span<RawEdge const>;

    /// Get string from string table
    [[nodiscard]] auto get_string(std::uint32_t offset, std::uint32_t length) const
        -> std::string_view;

    /// Verify the checksum
    [[nodiscard]] auto verify_checksum() const -> bool;

    /// Get the file size
    [[nodiscard]] auto file_size() const -> std::size_t { return size_; }

    /// Check if reader is valid
    [[nodiscard]] auto is_open() const -> bool { return data_ != nullptr; }

    /// Close the file
    auto close() -> void;

private:
    void* data_ = nullptr;
    std::size_t size_ = 0;
    int fd_ = -1;
};

} // namespace pup::index
