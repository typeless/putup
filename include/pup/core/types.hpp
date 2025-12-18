// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include <array>
#include <cstdint>

namespace pup {

/// Unique identifier for nodes in the dependency graph
/// 32-bit is sufficient for ~4 billion nodes (even AOSP has < 10M)
using NodeId = std::uint32_t;

/// Invalid node ID sentinel
inline constexpr auto INVALID_NODE_ID = NodeId { 0 };

/// Root node ID (parent of all top-level entries)
inline constexpr auto ROOT_NODE_ID = NodeId { 1 };

/// SHA-256 hash (32 bytes)
using Hash256 = std::array<std::byte, 32>;

/// Node types in the dependency graph (matches tup's model)
enum class NodeType : std::uint8_t {
    File = 0,         ///< Source file
    Command = 1,      ///< Build command
    Directory = 2,    ///< Directory
    Variable = 3,     ///< Configuration variable
    Generated = 4,    ///< Generated output file
    Ghost = 5,        ///< Placeholder for missing files
    Group = 6,        ///< Named group (bin) of files
    GeneratedDir = 7, ///< Auto-created output directory
    Root = 8,         ///< Project root
};

/// Dependency edge types
enum class LinkType : std::uint8_t {
    Normal = 1,   ///< Standard dependency (input->cmd, cmd->output)
    Sticky = 2,   ///< Explicit dependency from Tupfile
    Group = 3,    ///< Group membership link
    Implicit = 4, ///< Header dependencies discovered from .d files
};

/// Node state flags (bitmask)
enum class NodeFlags : std::uint16_t {
    None = 0,
    Modified = 1 << 0,  ///< Content changed since last build
    Created = 1 << 1,   ///< Newly created
    Deleted = 1 << 2,   ///< Marked for deletion
    ConfigDep = 1 << 3, ///< Depends on configuration
    Transient = 1 << 4, ///< Temporary file
};

/// Bitwise OR for NodeFlags
[[nodiscard]] constexpr auto operator|(NodeFlags a, NodeFlags b) -> NodeFlags
{
    return static_cast<NodeFlags>(
        static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b)
    );
}

/// Bitwise AND for NodeFlags
[[nodiscard]] constexpr auto operator&(NodeFlags a, NodeFlags b) -> NodeFlags
{
    return static_cast<NodeFlags>(
        static_cast<std::uint16_t>(a) & static_cast<std::uint16_t>(b)
    );
}

/// Check if flag is set
[[nodiscard]] constexpr auto has_flag(NodeFlags flags, NodeFlags flag) -> bool
{
    return (flags & flag) != NodeFlags::None;
}

} // namespace pup
