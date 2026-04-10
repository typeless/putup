// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstdint>

namespace pup {

/// Lightweight handle to an interned path in a PathPool.
/// A path is a chain of (parent PathId, basename StringId) entries.
///
/// Three reserved roots define the path domain:
///   Ungrounded (0) — path fragment with no root (intermediate form)
///   SourceRoot (1) — source tree root
///   BuildRoot  (2) — build tree root
///
/// Every grounded path chains back to SourceRoot or BuildRoot.
/// Ungrounded paths are intermediate forms used during parsing/expansion.
enum class PathId : std::uint32_t {
    Ungrounded = 0, ///< Fragment with no root (sentinel)
    SourceRoot = 1, ///< Source tree root
    BuildRoot = 2,  ///< Build tree root

    Root = Ungrounded, ///< Alias for Ungrounded
};

/// Check if a PathId is one of the three reserved roots.
[[nodiscard]]
constexpr auto is_root(PathId id) -> bool
{
    return id == PathId::Ungrounded || id == PathId::SourceRoot || id == PathId::BuildRoot;
}

[[nodiscard]]
constexpr auto to_underlying(PathId id) -> std::uint32_t
{
    return static_cast<std::uint32_t>(id);
}

[[nodiscard]]
constexpr auto make_path_id(std::uint32_t value) -> PathId
{
    return static_cast<PathId>(value);
}

} // namespace pup
