// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/path_id.hpp"
#include "pup/core/robin_hood_index.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <cstddef>
#include <optional>
#include <string_view>

namespace pup {

class Buf;
class StringPool;

/// Interning trie of structured paths.
/// Each path is a (parent PathId, basename StringId) pair — the join operator.
/// Deduplication guarantees: same (parent, name) always returns the same PathId.
/// Callers must normalize ".." before calling intern_path — it is interned as a literal component.
class PathPool final {
public:
    PathPool();
    ~PathPool();

    PathPool(PathPool const&) = delete;
    auto operator=(PathPool const&) -> PathPool& = delete;

    PathPool(PathPool&&) noexcept;
    auto operator=(PathPool&&) noexcept -> PathPool&;

    /// Join: intern a (parent, name) pair. Returns existing PathId if already interned.
    /// Empty name is a no-op: returns parent unchanged.
    [[nodiscard]]
    auto intern(PathId parent, StringId name) -> PathId;

    /// Parse a slash-separated path string into a PathId chain.
    /// Each component is interned via pool. "." and empty components are skipped.
    [[nodiscard]]
    auto intern_path(std::string_view path, StringPool& pool) -> PathId;

    /// Decompose: get the parent of a path.
    [[nodiscard]]
    auto parent(PathId id) const -> PathId;

    /// Decompose: get the basename of a path.
    [[nodiscard]]
    auto name(PathId id) const -> StringId;

    /// Materialize: reconstruct the full path string. For display/filesystem only.
    /// Skips the root component (Ungrounded/SourceRoot/BuildRoot).
    [[nodiscard]]
    auto to_string(PathId id, StringPool& pool) const -> StringId;

    /// Materialize: write full path into a Buf. No interning — for transient use.
    auto write(PathId id, Buf& buf, StringPool const& pool) const -> void;

    /// Walk to root: returns Ungrounded, SourceRoot, or BuildRoot.
    [[nodiscard]]
    auto root(PathId id) const -> PathId;

    /// Check if a path is grounded (root is SourceRoot or BuildRoot).
    [[nodiscard]]
    auto is_grounded(PathId id) const -> bool;

    /// Re-intern an ungrounded path chain under a root.
    /// Requires: root(id) == Ungrounded, root ∈ {SourceRoot, BuildRoot}.
    [[nodiscard]]
    auto ground(PathId id, PathId root) -> PathId;

    /// Parse a slash-separated path starting from a specific root.
    [[nodiscard]]
    auto intern_path(std::string_view path, StringPool& pool, PathId root) -> PathId;

    /// Lookup a slash-separated path without creating entries. Returns nullopt if any
    /// component is missing. Read-only counterpart of intern_path.
    [[nodiscard]]
    auto find_path(std::string_view path, StringPool const& pool, PathId root) const -> std::optional<PathId>;

    [[nodiscard]]
    auto size() const -> std::size_t;

    auto clear() -> void;

private:
    struct Entry {
        PathId parent = PathId::Ungrounded;
        StringId name = StringId::Empty;
    };

    Vec<Entry> entries_;
    // Central (parent, name) -> child index; a child's key is readable
    // from its own entry, so the index stores only the child.
    RobinHoodIndex children_;
};

} // namespace pup
