// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/paged_vec.hpp"
#include "pup/core/path_id.hpp"
#include "pup/core/sorted_id_vec.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pup {

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
    [[nodiscard]]
    auto to_string(PathId id, StringPool& pool) const -> StringId;

    [[nodiscard]]
    auto size() const -> std::size_t;

    auto clear() -> void;

private:
    struct Entry {
        PathId parent = PathId::Root;
        StringId name = StringId::Empty;
    };

    Vec<Entry> entries_;
    PagedVec<SortedPairVec> children_;
};

} // namespace pup
