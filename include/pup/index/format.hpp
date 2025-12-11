// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "pup/core/types.hpp"

#include <array>
#include <cstdint>

namespace pup::index {

/// Magic number for index file: "PUPI" (Pup Index)
inline constexpr auto INDEX_MAGIC = std::array<char, 4> { 'P', 'U', 'P', 'I' };

/// Current index format version
/// Version history:
///   1 - Initial format with full path strings
///   2 - Added name field for tup-style (parent_dir, name) identification
///   3 - Removed path field, only name stored (path reconstructed at load time)
inline constexpr auto INDEX_VERSION = std::uint32_t { 3 };

/// Index file header (64 bytes, packed)
struct alignas(8) RawHeader {
    std::array<char, 4> magic = INDEX_MAGIC; ///< "PUPI"
    std::uint32_t version = INDEX_VERSION;   ///< Format version

    std::uint32_t flags = 0;      ///< Reserved flags
    std::uint32_t file_count = 0; ///< Number of file entries

    std::uint32_t command_count = 0; ///< Number of command entries
    std::uint32_t edge_count = 0;    ///< Number of edge entries

    std::uint32_t string_table_size = 0; ///< Size of string table in bytes
    std::uint32_t reserved1 = 0;         ///< Padding

    std::uint64_t file_offset = 0;    ///< Offset to file entries
    std::uint64_t command_offset = 0; ///< Offset to command entries
    std::uint64_t edge_offset = 0;    ///< Offset to edge entries
    std::uint64_t string_offset = 0;  ///< Offset to string table
};

static_assert(sizeof(RawHeader) == 64, "RawHeader must be 64 bytes");

/// Raw file entry (96 bytes)
/// Represents source files, generated files, directories, groups, etc.
struct alignas(8) RawFileEntry {
    std::uint64_t id = 0;        ///< Node ID
    std::uint64_t parent_id = 0; ///< Parent directory node ID
    std::uint64_t src_id = 0;    ///< For generated files: source command ID

    std::int64_t mtime_sec = 0; ///< Modification time (seconds)

    std::uint64_t size = 0; ///< File size

    std::int32_t mtime_nsec = 0; ///< Modification time (nanoseconds)
    std::uint8_t type = 0;       ///< NodeType
    std::uint8_t flags_low = 0;  ///< NodeFlags (low byte)
    std::uint8_t flags_high = 0; ///< NodeFlags (high byte)
    std::uint8_t reserved1 = 0;  ///< Padding

    std::uint32_t name_offset = 0; ///< Offset into string table for basename
    std::uint32_t name_length = 0; ///< Length of name string

    std::uint32_t reserved2 = 0; ///< Reserved (was path_offset in v1-v2)
    std::uint32_t reserved3 = 0; ///< Reserved (was path_length in v1-v2)

    Hash256 content_hash = {}; ///< SHA-256 content hash
};

static_assert(sizeof(RawFileEntry) == 96, "RawFileEntry must be 96 bytes");

/// Raw command entry (64 bytes)
/// Represents build commands
struct alignas(8) RawCommandEntry {
    std::uint64_t id = 0;     ///< Node ID
    std::uint64_t dir_id = 0; ///< Directory where command runs

    std::uint32_t cmd_offset = 0; ///< Offset into string table for command
    std::uint32_t cmd_length = 0; ///< Length of command string

    std::uint32_t display_offset = 0; ///< Offset into string table for display text
    std::uint32_t display_length = 0; ///< Length of display string

    std::uint32_t env_offset = 0; ///< Offset into string table for env vars
    std::uint32_t env_length = 0; ///< Length of env string

    std::uint8_t flags = 0; ///< Command flags
    std::uint8_t reserved1 = 0;
    std::uint16_t reserved2 = 0;
    std::uint32_t reserved3 = 0;

    std::uint64_t reserved4 = 0; ///< Reserved for future use
    std::uint64_t reserved5 = 0; ///< Reserved for future use
};

static_assert(sizeof(RawCommandEntry) == 64, "RawCommandEntry must be 64 bytes");

/// Raw edge entry (24 bytes)
/// Represents dependencies between nodes
struct alignas(8) RawEdge {
    std::uint64_t from_id = 0; ///< Source node ID
    std::uint64_t to_id = 0;   ///< Destination node ID
    std::uint8_t type = 0;     ///< LinkType
    std::uint8_t reserved1 = 0;
    std::uint16_t reserved2 = 0;
    std::uint32_t group_cmd_id = 0; ///< For group edges: command that owns group
};

static_assert(sizeof(RawEdge) == 24, "RawEdge must be 24 bytes");

/// Index file footer (32 bytes)
/// Contains checksum of entire file (excluding footer)
struct alignas(8) RawFooter {
    Hash256 checksum = {}; ///< SHA-256 of file content before footer
};

static_assert(sizeof(RawFooter) == 32, "RawFooter must be 32 bytes");

/// Helper to get file size from entry
[[nodiscard]] inline auto get_file_size(RawFileEntry const& entry) -> std::uint64_t
{
    return entry.size;
}

/// Helper to set file size in entry
inline auto set_file_size(RawFileEntry& entry, std::uint64_t size) -> void
{
    entry.size = size;
}

/// Helper to get NodeFlags from entry
[[nodiscard]] inline auto get_node_flags(RawFileEntry const& entry) -> NodeFlags
{
    return static_cast<NodeFlags>(
        (static_cast<std::uint16_t>(entry.flags_high) << 8) | entry.flags_low);
}

/// Helper to set NodeFlags in entry
inline auto set_node_flags(RawFileEntry& entry, NodeFlags flags) -> void
{
    auto const value = static_cast<std::uint16_t>(flags);
    entry.flags_low = static_cast<std::uint8_t>(value & 0xFF);
    entry.flags_high = static_cast<std::uint8_t>(value >> 8);
}

/// Helper to get FileTime from entry
[[nodiscard]] inline auto get_mtime(RawFileEntry const& entry) -> FileTime
{
    return FileTime {
        .seconds = entry.mtime_sec,
        .nanoseconds = entry.mtime_nsec,
    };
}

/// Helper to set FileTime in entry
inline auto set_mtime(RawFileEntry& entry, FileTime const& mtime) -> void
{
    entry.mtime_sec = mtime.seconds;
    entry.mtime_nsec = mtime.nanoseconds;
}

} // namespace pup::index
