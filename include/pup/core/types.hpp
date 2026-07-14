// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <array>
#include <cstdint>

namespace pup {

/// Unique identifier for nodes in the dependency graph. The high 3 bits are a
/// kind tag (see node_id below), so index() yields a 29-bit slot — ~536M nodes
/// per kind (even AOSP has < 10M). IDs are minted monotonically and never
/// reused within a run.
using NodeId = std::uint32_t;

/// Sentinel for "no node exists" / "no parent" (top-level nodes have parent_dir = 0)
inline constexpr auto INVALID_NODE_ID = NodeId { 0 };
inline constexpr auto SOURCE_ROOT_ID = INVALID_NODE_ID;

/// Build root node ID (parent of Generated/Ghost nodes in variant builds)
inline constexpr auto BUILD_ROOT_ID = NodeId { 1 };

/// NodeId encoding namespace - groups all ID type detection and manipulation functions
namespace node_id {

/// Flag bits for different node types (mutually exclusive in high nibble)
inline constexpr auto COMMAND_FLAG = NodeId { 0x80000000 };
inline constexpr auto CONDITION_FLAG = NodeId { 0x40000000 };
inline constexpr auto PHI_FLAG = NodeId { 0x20000000 };

/// Check if ID refers to a file node (no flags set)
[[nodiscard]]
constexpr auto is_file(NodeId id) -> bool
{
    return id != 0 && (id & (COMMAND_FLAG | CONDITION_FLAG | PHI_FLAG)) == 0;
}

/// Check if ID refers to a command node
[[nodiscard]]
constexpr auto is_command(NodeId id) -> bool
{
    return (id & COMMAND_FLAG) != 0;
}

/// Check if ID refers to a condition node
[[nodiscard]]
constexpr auto is_condition(NodeId id) -> bool
{
    return (id & CONDITION_FLAG) != 0 && (id & COMMAND_FLAG) == 0;
}

/// Check if ID refers to a phi node
[[nodiscard]]
constexpr auto is_phi(NodeId id) -> bool
{
    return (id & PHI_FLAG) != 0 && (id & COMMAND_FLAG) == 0 && (id & CONDITION_FLAG) == 0;
}

/// Get array index from any node ID (strips flag bits)
[[nodiscard]]
constexpr auto index(NodeId id) -> std::size_t
{
    return static_cast<std::size_t>(id & ~(COMMAND_FLAG | CONDITION_FLAG | PHI_FLAG));
}

/// Create command ID from array index
[[nodiscard]]
constexpr auto make_command(std::size_t idx) -> NodeId
{
    return static_cast<NodeId>(idx) | COMMAND_FLAG;
}

/// Create condition ID from array index
[[nodiscard]]
constexpr auto make_condition(std::size_t idx) -> NodeId
{
    return static_cast<NodeId>(idx) | CONDITION_FLAG;
}

/// Create phi ID from array index
[[nodiscard]]
constexpr auto make_phi(std::size_t idx) -> NodeId
{
    return static_cast<NodeId>(idx) | PHI_FLAG;
}

} // namespace node_id

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
    Condition = 9,    ///< Conditional guard (ifeq/ifdef condition)
    Phi = 10,         ///< Merges outputs from conditional branches
};

/// Dependency edge types
enum class LinkType : std::uint8_t {
    Normal = 1,    ///< Standard dependency (input->cmd, cmd->output)
    Sticky = 2,    ///< Explicit dependency from Tupfile
    Group = 3,     ///< Group membership link
    Implicit = 4,  ///< Header dependencies discovered from .d files
    OrderOnly = 5, ///< Order-only dependency (must build first, but not a data dep)
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
[[nodiscard]]
constexpr auto operator|(NodeFlags a, NodeFlags b) -> NodeFlags
{
    return static_cast<NodeFlags>(
        static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b)
    );
}

/// Bitwise AND for NodeFlags
[[nodiscard]]
constexpr auto operator&(NodeFlags a, NodeFlags b) -> NodeFlags
{
    return static_cast<NodeFlags>(
        static_cast<std::uint16_t>(a) & static_cast<std::uint16_t>(b)
    );
}

/// Check if flag is set
[[nodiscard]]
constexpr auto has_flag(NodeFlags flags, NodeFlags flag) -> bool
{
    return (flags & flag) != NodeFlags::None;
}

} // namespace pup
