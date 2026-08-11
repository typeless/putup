// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "entry.hpp"
#include "format.hpp"
#include "pup/core/result.hpp"
#include "pup/platform/file_io.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace pup::index {

/// Memory-mapped index file handle
/// Plain data struct - use free functions to operate on it
struct IndexFile {
    pup::platform::MappedFile file;
};

/// Open an index file for reading
[[nodiscard]]
auto open_index(std::string_view path) -> Result<IndexFile>;

/// Check if a file is a valid index file (checks magic and version)
[[nodiscard]]
auto is_valid_index(std::string_view path) -> bool;

/// Read the entire index into memory
[[nodiscard]]
auto read_index(IndexFile const& f) -> Result<Index>;

/// Convenience: open and read index in one call
[[nodiscard]]
auto read_index(std::string_view path) -> Result<Index>;

/// What a previous build's record says about the paths in this tree: which ones it recorded as
/// sources, and which ones it produced. A version bump retracts the record's currency; #291 was
/// it retracting this along with it, leaving the next build to call its own outputs checked-in
/// source files.
struct PriorPaths {
    enum class Kind : std::uint8_t {
        NeverBuilt, ///< No record at all, so nothing this project produced can be on disk yet
        Known,      ///< Recovered whole
        Lost,       ///< A build happened here and what it produced cannot be recovered
    };

    Kind kind = Kind::NeverBuilt;
    Vec<StringId> sources;   ///< Sorted; empty unless kind == Known
    Vec<StringId> generated; ///< Sorted; empty unless kind == Known
};

/// Classify the paths an already-loaded record holds.
[[nodiscard]]
auto prior_paths(Index const& index) -> PriorPaths;

/// Classify the paths a record on disk holds, including one too old for `read_index`. Reads the
/// header, the file table and the string table and nothing else, so no command, signature or
/// recorded currency from a version putup no longer trusts can reach a caller through here. A
/// record failing any check -- magic, checksum, the readable version window, its own declared
/// layout -- comes back `Lost`, never partly read.
[[nodiscard]]
auto read_prior_paths(std::string_view path) -> PriorPaths;

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
