// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "format.hpp"
#include "pup/core/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pup::index {

/// In-memory file entry
struct FileEntry {
    NodeId id = 0;
    NodeId parent_id = 0;
    NodeId src_id = 0; ///< For generated: command that creates it

    NodeType type = NodeType::File;
    NodeFlags flags = NodeFlags::None;

    std::string name = {}; ///< Basename only (tup-style identification)
    std::string path = {}; ///< Full path (computed from parent_id/name chain, not serialized)
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0; ///< Modification time (nanoseconds since epoch)
    Hash256 content_hash = {};

    /// Convert to raw format for serialization
    [[nodiscard]]
    auto to_raw(std::uint32_t name_offset) const -> RawFileEntry;

    /// Create from raw format (path must be computed separately from parent chain)
    /// @param array_index 0-based position in file array (ID = array_index + 1)
    [[nodiscard]]
    static auto from_raw(
        RawFileEntry const& raw,
        std::string_view name_str,
        std::size_t array_index
    ) -> FileEntry;
};

/// In-memory command entry (v8)
/// Stores instruction pattern with %f/%o markers plus operand NodeIds.
/// Full command string is reconstructed on demand via get_command_string().
struct CommandEntry {
    NodeId id = 0;
    NodeId dir_id = 0; ///< Directory where command runs

    std::string instruction_pattern = {}; ///< Instruction pattern with %f/%o markers
    std::string display = {};             ///< Display text (from ^ ^ markers)
    std::string env = {};                 ///< Environment variables

    std::vector<NodeId> inputs = {};  ///< Input file operands (for %f expansion)
    std::vector<NodeId> outputs = {}; ///< Output file operands (for %o expansion)

    /// Convert to raw format for serialization
    [[nodiscard]]
    auto to_raw(
        std::uint32_t instruction_offset,
        std::uint32_t display_offset,
        std::uint32_t env_offset
    ) const -> RawCommandEntry;

    /// Create from raw format
    /// @param array_index 0-based position in command array (ID = node_id::make_command(array_index + 1))
    [[nodiscard]]
    static auto from_raw(
        RawCommandEntry const& raw,
        std::string_view instruction_pattern,
        std::string_view display_str,
        std::string_view env_str,
        std::vector<NodeId> inputs,
        std::vector<NodeId> outputs,
        std::size_t array_index
    ) -> CommandEntry;
};

/// In-memory edge
struct EdgeEntry {
    NodeId from = 0;
    NodeId to = 0;
    LinkType type = LinkType::Normal;
    NodeId group_cmd_id = 0; ///< For group edges

    /// Convert to raw format
    [[nodiscard]]
    auto to_raw() const -> RawEdge;

    /// Create from raw format
    [[nodiscard]]
    static auto from_raw(RawEdge const& raw) -> EdgeEntry;
};

/// Complete in-memory index
class Index {
public:
    Index() = default;

    /// Add a file entry
    auto add_file(FileEntry entry) -> void { files_.push_back(std::move(entry)); }

    /// Add a command entry
    auto add_command(CommandEntry entry) -> void { commands_.push_back(std::move(entry)); }

    /// Add an edge (also updates edge indices)
    auto add_edge(EdgeEntry entry) -> void;

    /// Get all file entries
    [[nodiscard]]
    auto files() const -> std::vector<FileEntry> const&
    {
        return files_;
    }
    [[nodiscard]]
    auto files() -> std::vector<FileEntry>&
    {
        return files_;
    }

    /// Get all command entries
    [[nodiscard]]
    auto commands() const -> std::vector<CommandEntry> const&
    {
        return commands_;
    }
    [[nodiscard]]
    auto commands() -> std::vector<CommandEntry>&
    {
        return commands_;
    }

    /// Get all edges
    [[nodiscard]]
    auto edges() const -> std::vector<EdgeEntry> const&
    {
        return edges_;
    }
    [[nodiscard]]
    auto edges() -> std::vector<EdgeEntry>&
    {
        return edges_;
    }

    /// Find file by ID
    [[nodiscard]]
    auto find_file_by_id(NodeId id) const -> FileEntry const*;

    /// Find command by ID
    [[nodiscard]]
    auto find_command_by_id(NodeId id) const -> CommandEntry const*;

    /// Find command by command string (requires prior build_command_lookup() call)
    /// @deprecated Use build_command_lookup() for explicit lookup map management
    [[nodiscard]]
    auto find_command_by_command(std::string const& cmd) const -> CommandEntry const*;

    /// Rebuild internal command lookup index (call after modifying commands or after load)
    auto rebuild_command_index() -> void;

    /// Get edges from a node (O(1) after build_edge_indices)
    [[nodiscard]]
    auto edges_from(NodeId id) const -> std::vector<EdgeEntry const*>;

    /// Get edges to a node (O(1) after build_edge_indices)
    [[nodiscard]]
    auto edges_to(NodeId id) const -> std::vector<EdgeEntry const*>;

    /// Build edge indices for O(1) lookup (call after loading all edges)
    auto build_edge_indices() -> void;

    /// Compute paths from parent_id/name chain (call after loading all files)
    auto compute_paths() -> void;

    /// Build parent->children index for Merkle tree traversal
    auto build_children_index() -> void;

    /// Get children of a directory (O(1) after build_children_index)
    [[nodiscard]]
    auto get_children(NodeId parent_id) const -> std::vector<NodeId>;

    /// Compute Merkle hashes for all directories (bottom-up)
    auto compute_merkle_hashes() -> void;

    /// Check if Merkle hashes have been computed (directories have non-zero hashes)
    [[nodiscard]]
    auto has_merkle_hashes() const -> bool;

    /// Clear all entries
    auto clear() -> void;

    /// Check if index is empty
    [[nodiscard]]
    auto empty() const -> bool
    {
        return files_.empty() && commands_.empty() && edges_.empty();
    }

    /// Get total entry counts
    [[nodiscard]]
    auto file_count() const -> std::size_t
    {
        return files_.size();
    }
    [[nodiscard]]
    auto command_count() const -> std::size_t
    {
        return commands_.size();
    }
    [[nodiscard]]
    auto edge_count() const -> std::size_t
    {
        return edges_.size();
    }

    /// Get index save time (nanoseconds since epoch)
    [[nodiscard]]
    auto save_time_ns() const -> std::int64_t
    {
        return save_time_ns_;
    }

    /// Set index save time (used by reader)
    auto set_save_time_ns(std::int64_t ns) -> void { save_time_ns_ = ns; }

private:
    std::vector<FileEntry> files_ = {};
    std::vector<CommandEntry> commands_ = {};
    std::vector<EdgeEntry> edges_ = {};

    // Edge indices for O(1) lookup (indices into edges_ vector)
    std::unordered_map<NodeId, std::vector<std::size_t>> edges_from_index_ = {};
    std::unordered_map<NodeId, std::vector<std::size_t>> edges_to_index_ = {};

    // Children index for Merkle tree traversal (parent_id -> child ids)
    std::unordered_map<NodeId, std::vector<NodeId>> children_index_ = {};

    // Command index for O(1) lookup by command string (index into commands_ vector)
    std::unordered_map<std::string, std::size_t> command_index_ = {};

    // Index save time (nanoseconds since epoch) for racy-clean detection
    std::int64_t save_time_ns_ = 0;

    [[nodiscard]]
    auto lookup_edges(
        std::unordered_map<NodeId, std::vector<std::size_t>> const& index,
        NodeId id
    ) const -> std::vector<EdgeEntry const*>;
};

/// Reconstruct full command string from instruction + operands.
/// Uses pattern expansion to replace %f, %o, %b, %B, etc. with actual paths.
/// @param index The index containing file entries for path lookup
/// @param cmd The command entry with instruction and operands
/// @return The fully expanded command string
[[nodiscard]]
auto get_command_string(Index const& index, CommandEntry const& cmd) -> std::string;

/// Build a lookup map from command strings to command entries.
/// This is O(N) reconstruction - call once when needed for change detection.
/// @param index The index to build lookup from
/// @return Map of command string -> command entry pointer
[[nodiscard]]
auto build_command_lookup(Index const& index)
    -> std::unordered_map<std::string, CommandEntry const*>;

} // namespace pup::index
