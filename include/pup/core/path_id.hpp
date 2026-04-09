// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstdint>

namespace pup {

/// Lightweight handle to an interned path in a PathPool.
/// A path is a chain of (parent PathId, basename StringId) entries.
/// PathId::Root represents the project root (analogous to StringId::Empty).
enum class PathId : std::uint32_t { Root = 0 };

[[nodiscard]]
constexpr auto is_root(PathId id) -> bool
{
    return id == PathId::Root;
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
