// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstdint>
#include <functional>

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

} // namespace pup

namespace std {

template<>
struct hash<pup::StringId> {
    auto operator()(pup::StringId id) const noexcept -> std::size_t
    {
        return std::hash<std::uint32_t> {}(pup::to_underlying(id));
    }
};

} // namespace std
