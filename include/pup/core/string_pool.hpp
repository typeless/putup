// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace pup {

/// Arena-based string pool with deduplication
/// Strings are stored contiguously and referenced by StringId handles.
/// This eliminates per-lookup allocations and reduces memory overhead.
class StringPool {
public:
    StringPool();
    ~StringPool();

    StringPool(StringPool const&) = delete;
    auto operator=(StringPool const&) -> StringPool& = delete;

    StringPool(StringPool&&) noexcept;
    auto operator=(StringPool&&) noexcept -> StringPool&;

    /// Intern a string, returning its ID
    /// If the string already exists, returns the existing ID (deduplication).
    /// Empty strings always return StringId::Empty.
    [[nodiscard]]
    auto intern(std::string_view str) -> StringId;

    /// Get string by ID
    /// Returns empty string_view for StringId::Empty or invalid IDs.
    [[nodiscard]]
    auto get(StringId id) const -> std::string_view;

    /// Find existing interned string without adding
    /// Returns StringId::Empty if not found.
    [[nodiscard]]
    auto find(std::string_view str) const -> StringId;

    /// Get number of interned strings (excluding empty)
    [[nodiscard]]
    auto size() const -> std::size_t;

    /// Clear all interned strings
    auto clear() -> void;

    /// Reserve capacity for expected number of strings
    auto reserve(std::size_t count) -> void;

private:
    struct Meta {
        std::uint32_t hash = 0;
        std::uint16_t displacement = 0;
    };

    auto key_at(std::size_t slot) const -> std::string_view;
    auto probe_find(std::uint32_t h, std::string_view key) const -> StringId;
    auto probe_insert(std::uint32_t h, StringId id) -> void;
    auto grow() -> void;

    std::deque<std::string> storage_;
    Meta* meta_ = nullptr;
    StringId* values_ = nullptr;
    std::size_t index_capacity_ = 0;
    std::size_t index_count_ = 0;
};

} // namespace pup
