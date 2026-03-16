// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "entry.hpp"
#include "format.hpp"
#include "pup/core/result.hpp"
#include "pup/platform/file_io.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace pup::index {

/// Memory-mapped index file handle
/// Plain data struct - use free functions to operate on it
struct IndexFile {
    pup::platform::MappedFile file;
};

/// Open an index file for reading
[[nodiscard]]
auto open_index(std::string const& path) -> Result<IndexFile>;

/// Check if a file is a valid index file (checks magic and version)
[[nodiscard]]
auto is_valid_index(std::string const& path) -> bool;

/// Read the entire index into memory
[[nodiscard]]
auto read_index(IndexFile const& f) -> Result<Index>;

/// Convenience: open and read index in one call
[[nodiscard]]
auto read_index(std::string const& path) -> Result<Index>;

/// Get the header
[[nodiscard]]
auto index_header(IndexFile const& f) -> RawHeader const*;

/// Get file entries as raw span
[[nodiscard]]
auto index_raw_files(IndexFile const& f) -> std::span<RawFileEntry const>;

/// Get command entries as raw span
[[nodiscard]]
auto index_raw_commands(IndexFile const& f) -> std::span<RawCommandEntry const>;

/// Get edge entries as raw span
[[nodiscard]]
auto index_raw_edges(IndexFile const& f) -> std::span<RawEdge const>;

/// Get string from string table (length-prefixed)
[[nodiscard]]
auto index_get_string(IndexFile const& f, std::uint32_t offset) -> std::string_view;

/// Get operands for a command (v8)
/// Returns pair of {input NodeIds, output NodeIds}
[[nodiscard]]
auto index_get_operands(IndexFile const& f, std::size_t cmd_index)
    -> std::pair<std::vector<NodeId>, std::vector<NodeId>>;

/// Verify the checksum
[[nodiscard]]
auto index_verify_checksum(IndexFile const& f) -> bool;

/// Get the file size
[[nodiscard]]
inline auto index_file_size(IndexFile const& f) -> std::size_t
{
    return f.file.size();
}

/// Check if index file is valid/open
[[nodiscard]]
inline auto index_is_open(IndexFile const& f) -> bool
{
    return f.file.is_open();
}

} // namespace pup::index
