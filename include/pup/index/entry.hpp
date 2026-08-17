// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "format.hpp"
#include "pup/core/arena.hpp"
#include "pup/core/node_id_map.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"

#include <string_view>

namespace pup::index {

/// In-memory file entry
struct FileEntry {
    NodeId id = 0;
    NodeId parent_id = 0;
    NodeId src_id = 0; ///< For generated: command that creates it

    NodeType type = NodeType::File;
    NodeFlags flags = NodeFlags::None;

    StringId name = StringId::Empty; ///< Basename only (tup-style identification)
    StringId path = StringId::Empty; ///< Full path (computed from parent_id/name chain, not serialized)
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0; ///< Modification time (nanoseconds since epoch)
    Hash256 content_hash = {};

    /// Convert to raw format for serialization
    [[nodiscard]]
    auto to_raw(std::uint32_t name_offset) const -> RawFileEntry;

    /// Create from raw format (path must be computed separately from parent chain)
    /// @param array_index 0-based position in file array (ID = array_index + 1)
    /// @return IndexDamaged if the recorded type byte names no NodeType this build knows
    [[nodiscard]]
    static auto from_raw(
        RawFileEntry const& raw,
        std::string_view name_str,
        std::size_t array_index
    ) -> Result<FileEntry>;
};

/// In-memory command entry (v8)
/// Stores instruction pattern with %f/%o markers plus operand NodeIds.
/// Full command string is reconstructed on demand via get_command_string().
struct CommandEntry {
    NodeId id = 0;
    NodeId dir_id = 0; ///< Directory where command runs

    StringId instruction_pattern = StringId::Empty; ///< Instruction pattern with %f/%o markers
    StringId display = StringId::Empty;             ///< Display text (from ^ ^ markers)
    StringId env = StringId::Empty;                 ///< Environment variables

    Hash256 key = {};       ///< Which rule this is: the cross-build join key
    Hash256 signature = {}; ///< What it will do: changes mean re-run, not a different rule
    /// Its last run is not evidence that its outputs are current: it exited nonzero, or a build
    /// carried this record past a change to one of its deps without re-running it.
    bool must_rerun = false;

    Vec<NodeId> inputs = {};  ///< Input file operands (for %f expansion)
    Vec<NodeId> outputs = {}; ///< Output file operands (for %o expansion)

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
        Vec<NodeId> inputs,
        Vec<NodeId> outputs,
        std::size_t array_index
    ) -> CommandEntry;
};

/// In-memory edge
struct EdgeEntry {
    NodeId from = 0;
    NodeId to = 0;
    LinkType type = LinkType::Normal;

    /// Convert to raw format
    [[nodiscard]]
    auto to_raw() const -> RawEdge;

    /// Create from raw format
    /// @return IndexDamaged if the recorded type byte names no LinkType this build knows
    [[nodiscard]]
    static auto from_raw(RawEdge const& raw) -> Result<EdgeEntry>;
};

/// Complete in-memory index
class Index final {
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
    auto files() const -> Vec<FileEntry> const&
    {
        return files_;
    }
    [[nodiscard]]
    auto files() -> Vec<FileEntry>&
    {
        return files_;
    }

    /// Get all command entries
    [[nodiscard]]
    auto commands() const -> Vec<CommandEntry> const&
    {
        return commands_;
    }
    [[nodiscard]]
    auto commands() -> Vec<CommandEntry>&
    {
        return commands_;
    }

    /// Get all edges
    [[nodiscard]]
    auto edges() const -> Vec<EdgeEntry> const&
    {
        return edges_;
    }
    [[nodiscard]]
    auto edges() -> Vec<EdgeEntry>&
    {
        return edges_;
    }

    /// Find file by ID
    [[nodiscard]]
    auto find_file_by_id(NodeId id) const -> FileEntry const*;

    /// Find command by ID
    [[nodiscard]]
    auto find_command_by_id(NodeId id) const -> CommandEntry const*;

    /// Get edges from a node (O(1) after build_edge_indices)
    [[nodiscard]]
    auto edges_from(NodeId id) const -> Vec<EdgeEntry const*>;

    /// Get edges to a node (O(1) after build_edge_indices)
    [[nodiscard]]
    auto edges_to(NodeId id) const -> Vec<EdgeEntry const*>;

    /// Build edge indices for O(1) lookup (call after loading all edges)
    auto build_edge_indices() -> void;

    /// Compute paths from parent_id/name chain (call after loading all files)
    auto compute_paths() -> void;

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
    Vec<FileEntry> files_ = {};
    Vec<CommandEntry> commands_ = {};
    Vec<EdgeEntry> edges_ = {};

    // Edge indices (indices into edges_ vector)
    Arena32 edge_arena_;
    NodeIdArenaIndex edges_from_index_;
    NodeIdArenaIndex edges_to_index_;

    // Index save time (nanoseconds since epoch) for racy-clean detection
    std::int64_t save_time_ns_ = 0;

    [[nodiscard]]
    auto lookup_edges(NodeIdArenaIndex const& index, NodeId id) const
        -> Vec<EdgeEntry const*>;
};

/// An index's file table keyed by path, ordered by interning handle.
///
/// A type of its own rather than the bare pair vector: `pup::cli::PathIdMap` keys the index
/// being built the same way, in the same order, and while the two were spelled as aliases of
/// a pair vector nothing said which one a function wanted.
struct FilesByPath {
    Vec<std::pair<StringId, FileEntry const*>> entries = {};

    /// The entry recorded at `path`, or nullptr. Keyed by handle, so `path` must be interned
    /// in the same pool as the index's.
    [[nodiscard]]
    auto find(StringId path) const -> FileEntry const*;
};

/// Key `index`'s file table by path. An entry with no path is left out: an index carries them
/// and a path key cannot address them.
/// @param index The index to key; its entries must outlive the result, which borrows them
[[nodiscard]]
auto files_by_path(Index const& index) -> FilesByPath;

/// Reconstruct full command string from instruction + operands.
/// Uses pattern expansion to replace %f, %o, %b, %B, etc. with actual paths.
/// @param index The index containing file entries for path lookup
/// @param cmd The command entry with instruction and operands
/// @return The fully expanded command string
[[nodiscard]]
auto get_command_string(Index const& index, CommandEntry const& cmd) -> StringId;

} // namespace pup::index
