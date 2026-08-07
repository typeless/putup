// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/types.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace pup::index {

/// Magic number for index file: "PUPI" (Pup Index)
inline constexpr auto INDEX_MAGIC = std::array<char, 4> { 'P', 'U', 'P', 'I' };

/// Current index format version
/// Version history:
///   1 - Initial format with full path strings
///   2 - Added name field for tup-style (parent_dir, name) identification
///   3 - Removed path field, only name stored (path reconstructed at load time)
///   4 - (removed) Directory Merkle hashes - not useful for change detection
///   5 - Removed mtime fields, change detection uses size + content hash only
///   6 - Compact format: 32-bit IDs/offsets, length-prefixed strings
///   7 - Separate ID spaces: files 1..N, commands 0x80000001...; ID field removed
///   8 - Template deduplication: commands store template + operands, reconstruct lazily
///   9 - Stat cache: mtime_ns in file entries, save_time_ns in header for racy-clean detection
///  10 - Remove unused group_cmd_id field from edges (reserved bytes expanded)
///  11 - Command structural identity: per-command hash folding command text + the values
///       of vars it depends on (sticky/exported), replacing rendered-string identity
///  12 - NodeId kind moved to a 2-bit tag in bits 30-31 (commands 0x40000001...);
///       persisted NodeIds from older versions decode differently, so v11 indexes
///       are rejected and rebuilt
///  13 - Ghost entries that exist on disk record path/size/mtime/hash so change
///       detection tracks foreign inputs (e.g. the variant's tup.config); v12
///       indexes lack that data and would never notice such changes
///  14 - Guard-unsatisfied commands are no longer serialized; v13 indexes carry
///       their identities and would mask reactivated output-less commands as
///       already known, so they never run (issue #118)
///  15 - Command identity folds the rule's source directory; v14 identities are
///       directory-blind, so identical rules in sibling directories collide and
///       resolve to one another through the identity join (issue #167). Also
///       covers OrderOnly edges, persisted since issue #166 and load-bearing for
///       removal routing; v14 indexes predate them and would lose one removal.
///  16 - Glob matches are ordered by path rather than by interning (readdir)
///       order, so %f and the identity hashing it changed for every glob-fed
///       command (issue #171); v15 identities no longer join, and a scoped build
///       would merge the stale twin back in beside the new one.
///  17 - Glob expansion merges filesystem and generated matches into one list
///       instead of letting a filesystem match suppress the generated ones, so
///       both the membership and the order of %f changed for any pattern a
///       generated file matches (issues #177, #178); v16 identities no longer
///       join for those commands.
///  18 - A dep-scan command's identity folds its parent command's identity. Two
///       compiles of one source with equal flags render byte-identical scans, so
///       v17 gave them one identity and the join between them was arbitrary
///       (issue #170); every dep-scan command's identity changes.
///  19 - The single `identity` splits into `key` (which rule this is) and
///       `signature` (what it will do), because one value cannot both stay stable
///       across a recipe edit and change when the recipe changes (issue #188).
///       v18 entries carry neither field.
///  20 - A command records whether its last run failed. v19 inferred that from the
///       output being absent, which is false whenever a failing command writes its
///       output before failing, so the failure was forgotten (issue #187).
///  21 - A build records only the state of files it examined; one it neither examined
///       nor produced keeps what the last build that looked at it recorded (issue
///       #288). A v20 index may already assert currency for a file no build ever
///       verified, and no later build detects that, so v20 is not read.
/// Bump when a stale record would wrong-join — join a key whose meaning changed; a change that
/// only changes keys no-joins, and one re-run per affected command repairs it (#333, #335, #343).
inline constexpr auto INDEX_VERSION = std::uint32_t { 21 };

/// The oldest version whose `RawHeader` and `RawFileEntry` bytes mean what today's mean, so a
/// record that old still says which paths it recorded as sources and which as generated even
/// though nothing else in it may be trusted (issue #291). v9 is where both structs last changed.
/// Nothing but that classification is ever read from a version below `INDEX_VERSION`.
inline constexpr auto INDEX_LAYOUT_FLOOR = std::uint32_t { 9 };

static_assert(INDEX_LAYOUT_FLOOR <= INDEX_VERSION, "the readable window runs backwards from the current version");

/// Index file header (56 bytes) - v9
struct alignas(8) RawHeader {
    std::array<char, 4> magic = INDEX_MAGIC; ///< "PUPI"
    std::uint32_t version = INDEX_VERSION;   ///< Format version
    std::uint32_t file_count = 0;            ///< Number of file entries
    std::uint32_t command_count = 0;         ///< Number of command entries
    std::uint32_t edge_count = 0;            ///< Number of edge entries
    std::uint32_t string_table_size = 0;     ///< Size of string table in bytes
    std::uint32_t file_offset = 0;           ///< Offset to file entries
    std::uint32_t command_offset = 0;        ///< Offset to command entries
    std::uint32_t edge_offset = 0;           ///< Offset to edge entries
    std::uint32_t operand_table_offset = 0;  ///< Offset to operand offset table (v8)
    std::uint32_t operand_data_offset = 0;   ///< Offset to operand data (v8)
    std::uint32_t string_offset = 0;         ///< Offset to string table
    std::int64_t save_time_ns = 0;           ///< Index save time (nanoseconds since epoch, v9)
};

static_assert(sizeof(RawHeader) == 56, "RawHeader must be 56 bytes");

/// Raw file entry (64 bytes) - v9
/// Represents source files, generated files, directories, groups, etc.
/// Node ID is computed from array position: id = array_index (files start at 1)
struct alignas(8) RawFileEntry {
    std::uint32_t parent_id = 0;    ///< Parent directory node ID
    std::uint32_t src_id = 0;       ///< For generated files: source command ID
    std::uint32_t name_offset = 0;  ///< Offset into string table (length-prefixed)
    std::uint8_t type = 0;          ///< NodeType
    std::uint8_t flags_low = 0;     ///< NodeFlags (low byte)
    std::uint8_t flags_high = 0;    ///< NodeFlags (high byte)
    std::uint8_t reserved_byte = 0; ///< Padding for 8-byte alignment of size
    std::uint64_t size = 0;         ///< File size
    std::int64_t mtime_ns = 0;      ///< Modification time (nanoseconds since epoch, v9)
    Hash256 content_hash = {};      ///< SHA-256 content hash
};

static_assert(
    sizeof(RawFileEntry) == 64,
    "RawFileEntry changed size: bump INDEX_VERSION, and wire the new state's three legs "
    "— record it here, compare it in the change-detection walk, and route it "
    "(collect_affected_commands) — before updating this number. Note a category carried in "
    "`flags` changes no size and this assert will not fire for it. This is also where "
    "INDEX_LAYOUT_FLOOR stops being true: raise it to the new version"
);

/// Raw command entry (16 bytes) - v8
/// Represents build commands. In v8, cmd_offset points to template string (with %f/%o patterns)
/// rather than fully-expanded command. Operands stored in separate operand section.
/// Node ID is computed from array position: id = array_index | COMMAND_ID_FLAG
struct alignas(8) RawCommandEntry {
    std::uint32_t dir_id = 0;         ///< Directory where command runs
    std::uint32_t cmd_offset = 0;     ///< Offset to template string with %f/%o patterns (v8)
    std::uint32_t display_offset = 0; ///< Display text offset (length-prefixed)
    std::uint32_t env_offset = 0;     ///< Environment variables offset (length-prefixed)
    std::uint32_t flags = 0;          ///< CommandFlag bits (v20)
    std::uint32_t reserved = 0;       ///< Explicit tail padding; keeps serialized bytes deterministic
    Hash256 key = {};                 ///< Which rule this is, for the cross-build join (v19)
    Hash256 signature = {};           ///< What it will do, for deciding whether to re-run (v19)
};

/// A bit in RawCommandEntry::flags. Each bit is a category of recorded state that changes no
/// size, so the assert below cannot see it; `flag_category` is what asks for the three legs
/// instead, and it does not compile until a new flag answers.
enum class CommandFlag : std::uint32_t {
    /// Its last run is not evidence that its outputs are current, so it must run again whatever
    /// they look like: either it exited nonzero, or a build carried the record past a change to
    /// one of its deps without re-running it.
    MustRerun = 1,
};

/// Which category of recorded state this flag carries, named as its requirements group is. The
/// switch is exhaustive under -Wswitch, so adding a flag without answering is a build error.
[[nodiscard]]
constexpr auto flag_category(CommandFlag flag) -> std::string_view
{
    switch (flag) {
    case CommandFlag::MustRerun:
        return "must-rerun";
    }
    return {};
}

// Nothing calls flag_category — the switch is the point, so this is what keeps it compiled.
static_assert(!flag_category(CommandFlag::MustRerun).empty(), "a flag names a category or it is not a category");

[[nodiscard]]
constexpr auto to_underlying(CommandFlag flag) -> std::uint32_t
{
    return static_cast<std::uint32_t>(flag);
}

static_assert(
    to_underlying(CommandFlag::MustRerun) == 1,
    "a flag's value is on-disk format, not implementation: changing one needs an INDEX_VERSION bump"
);

// Widening this record adds a category of recorded state, and a category is only useful once all
// three of its legs exist: written here, compared where staleness is decided, and routed so the
// commands that depend on it are scheduled. A category with a missing leg is a silent wrong build,
// which is the shape #189 catalogues — so the size is fixed deliberately, to stop a new field
// reaching the index before someone has answered for all three.
static_assert(
    sizeof(RawCommandEntry) == 88,
    "RawCommandEntry changed size: bump INDEX_VERSION, and wire the new state's three legs "
    "— record it here, compare it (compute_command_signature, the must_rerun channel, or the "
    "identity key if it changes which rule this is), "
    "and route it (collect_affected_commands) — before updating this number. A category carried "
    "as a bit in `flags` changes no size and will not trip this assert: CommandFlag::MustRerun "
    "landed that way, which is why every flag has to name its category in flag_category"
);

/// Raw edge entry (16 bytes)
/// Represents dependencies between nodes
struct alignas(8) RawEdge {
    std::uint32_t from_id = 0;                 ///< Source node ID
    std::uint32_t to_id = 0;                   ///< Destination node ID
    std::uint8_t type = 0;                     ///< LinkType
    std::array<std::uint8_t, 7> reserved = {}; ///< Padding
};

static_assert(sizeof(RawEdge) == 16, "RawEdge must be 16 bytes");

/// Index file footer (32 bytes)
/// Contains checksum of entire file (excluding footer)
struct alignas(8) RawFooter {
    Hash256 checksum = {}; ///< SHA-256 of file content before footer
};

static_assert(sizeof(RawFooter) == 32, "RawFooter must be 32 bytes");

/// Helper to get NodeFlags from entry
[[nodiscard]]
inline auto get_node_flags(RawFileEntry const& entry) -> NodeFlags
{
    return static_cast<NodeFlags>(
        (static_cast<std::uint16_t>(entry.flags_high) << 8) | entry.flags_low
    );
}

/// Helper to set NodeFlags in entry
inline auto set_node_flags(RawFileEntry& entry, NodeFlags flags) -> void
{
    auto const value = static_cast<std::uint16_t>(flags);
    entry.flags_low = static_cast<std::uint8_t>(value & 0xFF);
    entry.flags_high = static_cast<std::uint8_t>(value >> 8);
}

} // namespace pup::index
