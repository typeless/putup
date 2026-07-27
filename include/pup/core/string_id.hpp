// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <compare>
#include <cstdint>

namespace pup {

/// Lightweight handle to an interned string
/// 0 represents the empty string (not invalid - empty string is a valid value)
enum class StringId : std::uint32_t { Empty = 0 };

/// Check if StringId represents the empty string
[[nodiscard]]
constexpr auto is_empty(StringId id) -> bool
{
    return id == StringId::Empty;
}

/// Convert StringId to raw value (for serialization)
[[nodiscard]]
constexpr auto to_underlying(StringId id) -> std::uint32_t
{
    return static_cast<std::uint32_t>(id);
}

/// Create StringId from raw value (for deserialization)
[[nodiscard]]
constexpr auto make_string_id(std::uint32_t value) -> StringId
{
    return static_cast<StringId>(value);
}

/// Relative order of two StringIds is deleted, so each site states which order it
/// means: `handle_less` for a set, or a pool projection for anything observable.
/// The built-in enum comparison is interning order, and the two spellings were
/// indistinguishable — issues #171, #175, #183 and #184 were all that confusion.
/// <=> is deleted too, or a defaulted operator<=> on any struct holding a StringId
/// would order by handle without naming it.
auto operator<(StringId, StringId) -> bool = delete;
auto operator>(StringId, StringId) -> bool = delete;
auto operator<=(StringId, StringId) -> bool = delete;
auto operator>=(StringId, StringId) -> bool = delete;
auto operator<=>(StringId, StringId) -> std::strong_ordering = delete;

/// Order by interning handle. Valid only where the order is unobservable —
/// membership, dedup, binary search against a handle-keyed range.
inline constexpr auto handle_less = [](StringId a, StringId b) {
    return to_underlying(a) < to_underlying(b);
};

} // namespace pup
