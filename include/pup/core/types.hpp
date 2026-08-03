// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <array>
#include <cstdint>

namespace pup {

/// Unique identifier for nodes in the dependency graph. The high 2 bits are a
/// kind tag (see node_id below), so index() yields a 30-bit slot — ~1.07B nodes
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

/// Node kind, stored in the top 2 bits of a NodeId. Every uint32 decodes to
/// exactly one kind, so mixed-flag junk states are unrepresentable.
enum class Tag : NodeId {
    File = 0,
    Command = 1,
    Condition = 2,
    Phi = 3,
};

inline constexpr auto TAG_SHIFT = NodeId { 30 };

/// Get the kind tag of any node ID
[[nodiscard]]
constexpr auto tag(NodeId id) -> Tag
{
    return static_cast<Tag>(id >> TAG_SHIFT);
}

/// Create an ID of the given kind from an array index
[[nodiscard]]
constexpr auto make(Tag t, std::size_t idx) -> NodeId
{
    return static_cast<NodeId>(idx) | (static_cast<NodeId>(t) << TAG_SHIFT);
}

/// Check if ID refers to a file node
[[nodiscard]]
constexpr auto is_file(NodeId id) -> bool
{
    return id != 0 && tag(id) == Tag::File;
}

/// Check if ID refers to a command node
[[nodiscard]]
constexpr auto is_command(NodeId id) -> bool
{
    return tag(id) == Tag::Command;
}

/// Check if ID refers to a condition node
[[nodiscard]]
constexpr auto is_condition(NodeId id) -> bool
{
    return tag(id) == Tag::Condition;
}

/// Check if ID refers to a phi node
[[nodiscard]]
constexpr auto is_phi(NodeId id) -> bool
{
    return tag(id) == Tag::Phi;
}

/// Get array index from any node ID (strips the tag)
[[nodiscard]]
constexpr auto index(NodeId id) -> std::size_t
{
    return static_cast<std::size_t>(id & ((NodeId { 1 } << TAG_SHIFT) - 1));
}

/// Create command ID from array index
[[nodiscard]]
constexpr auto make_command(std::size_t idx) -> NodeId
{
    return make(Tag::Command, idx);
}

/// Create condition ID from array index
[[nodiscard]]
constexpr auto make_condition(std::size_t idx) -> NodeId
{
    return make(Tag::Condition, idx);
}

/// Create phi ID from array index
[[nodiscard]]
constexpr auto make_phi(std::size_t idx) -> NodeId
{
    return make(Tag::Phi, idx);
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

/// What an edge does for code that routes changes, as opposed to what it records.
enum class LinkRole : std::uint8_t {
    DataFlow, ///< Carries content: a change at the tail is a change at the head
    Ordering, ///< Sequences without carrying content
    Identity, ///< Contributes to what a command is, not to what it reads
    Unknown,  ///< Not a link type this build knows; joins no mask, so it routes nothing
};

/// The one place a new LinkType must be classified: no `default`, so -Wswitch makes
/// omitting one a build error here instead of a silent exclusion from every mask.
[[nodiscard]]
constexpr auto link_role(LinkType type) -> LinkRole
{
    switch (type) {
    case LinkType::Normal:
    case LinkType::Group:
    case LinkType::Implicit:
        return LinkRole::DataFlow;
    case LinkType::OrderOnly:
        return LinkRole::Ordering;
    case LinkType::Sticky:
        return LinkRole::Identity;
    }
    // Reached only from an out-of-range value in a corrupt index, which must route nothing.
    return LinkRole::Unknown;
}

/// One command that must run before another, on evidence no edge in the graph carries. A
/// discovered dependency is recorded only in the index, so both the router that decides who runs
/// and the scheduler that decides in what order have to be told about it separately (#276, #277).
struct OrderingEdge {
    NodeId producer = INVALID_NODE_ID;
    NodeId consumer = INVALID_NODE_ID;
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
