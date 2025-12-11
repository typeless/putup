// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

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

    std::string path = {}; ///< Full path (transitional, will be removed)
    std::string name = {}; ///< Basename only (tup-style identification)
    std::uint64_t size = 0;
    FileTime mtime = {};
    Hash256 content_hash = {};

    /// Convert to raw format for serialization
    [[nodiscard]] auto to_raw(std::uint32_t path_offset, std::uint32_t name_offset) const -> RawFileEntry;

    /// Create from raw format
    [[nodiscard]] static auto from_raw(RawFileEntry const& raw, std::string_view path_str, std::string_view name_str)
        -> FileEntry;
};

/// In-memory command entry
struct CommandEntry {
    NodeId id = 0;
    NodeId dir_id = 0; ///< Directory where command runs

    std::string command = {}; ///< Full command string
    std::string display = {}; ///< Display text (from ^ ^ markers)
    std::string env = {};     ///< Environment variables

    std::uint8_t flags = 0;

    /// Convert to raw format for serialization
    [[nodiscard]] auto to_raw(
        std::uint32_t cmd_offset,
        std::uint32_t display_offset,
        std::uint32_t env_offset) const -> RawCommandEntry;

    /// Create from raw format
    [[nodiscard]] static auto from_raw(
        RawCommandEntry const& raw,
        std::string_view cmd_str,
        std::string_view display_str,
        std::string_view env_str) -> CommandEntry;
};

/// In-memory edge
struct EdgeEntry {
    NodeId from = 0;
    NodeId to = 0;
    LinkType type = LinkType::Normal;
    NodeId group_cmd_id = 0; ///< For group edges

    /// Convert to raw format
    [[nodiscard]] auto to_raw() const -> RawEdge;

    /// Create from raw format
    [[nodiscard]] static auto from_raw(RawEdge const& raw) -> EdgeEntry;
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
    [[nodiscard]] auto files() const -> std::vector<FileEntry> const& { return files_; }
    [[nodiscard]] auto files() -> std::vector<FileEntry>& { return files_; }

    /// Get all command entries
    [[nodiscard]] auto commands() const -> std::vector<CommandEntry> const& { return commands_; }
    [[nodiscard]] auto commands() -> std::vector<CommandEntry>& { return commands_; }

    /// Get all edges
    [[nodiscard]] auto edges() const -> std::vector<EdgeEntry> const& { return edges_; }
    [[nodiscard]] auto edges() -> std::vector<EdgeEntry>& { return edges_; }

    /// Find file by path
    [[nodiscard]] auto find_file(std::string_view path) const -> FileEntry const*;

    /// Find file by ID
    [[nodiscard]] auto find_file_by_id(NodeId id) const -> FileEntry const*;

    /// Find command by ID
    [[nodiscard]] auto find_command_by_id(NodeId id) const -> CommandEntry const*;

    /// Get edges from a node (O(1) after build_edge_indices)
    [[nodiscard]] auto edges_from(NodeId id) const -> std::vector<EdgeEntry const*>;

    /// Get edges to a node (O(1) after build_edge_indices)
    [[nodiscard]] auto edges_to(NodeId id) const -> std::vector<EdgeEntry const*>;

    /// Build edge indices for O(1) lookup (call after loading all edges)
    auto build_edge_indices() -> void;

    /// Clear all entries
    auto clear() -> void;

    /// Check if index is empty
    [[nodiscard]] auto empty() const -> bool
    {
        return files_.empty() && commands_.empty() && edges_.empty();
    }

    /// Get total entry counts
    [[nodiscard]] auto file_count() const -> std::size_t { return files_.size(); }
    [[nodiscard]] auto command_count() const -> std::size_t { return commands_.size(); }
    [[nodiscard]] auto edge_count() const -> std::size_t { return edges_.size(); }

private:
    std::vector<FileEntry> files_ = {};
    std::vector<CommandEntry> commands_ = {};
    std::vector<EdgeEntry> edges_ = {};

    // Edge indices for O(1) lookup (indices into edges_ vector)
    std::unordered_map<NodeId, std::vector<std::size_t>> edges_from_index_ = {};
    std::unordered_map<NodeId, std::vector<std::size_t>> edges_to_index_ = {};
};

} // namespace pup::index
