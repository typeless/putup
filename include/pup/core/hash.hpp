// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "result.hpp"
#include "types.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

namespace pup {

/// SHA-256 hasher (Git's block implementation)
class Sha256 {
public:
    Sha256();
    ~Sha256();

    Sha256(Sha256 const&) = delete;
    auto operator=(Sha256 const&) -> Sha256& = delete;
    Sha256(Sha256&&) noexcept;
    auto operator=(Sha256&&) noexcept -> Sha256&;

    /// Add data to the hash
    auto update(std::span<std::byte const> data) -> void;

    /// Add string data to the hash
    auto update(std::string_view data) -> void;

    /// Finalize and return the hash
    [[nodiscard]] auto finalize() -> Hash256;

    /// Reset to initial state for reuse
    auto reset() -> void;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

/// Compute SHA-256 hash of a byte span
[[nodiscard]] auto sha256(std::span<std::byte const> data) -> Hash256;

/// Compute SHA-256 hash of a string
[[nodiscard]] auto sha256(std::string_view data) -> Hash256;

/// Compute SHA-256 hash of a file
[[nodiscard]] auto sha256_file(std::filesystem::path const& path) -> Result<Hash256>;

/// Convert hash to hex string (lowercase)
[[nodiscard]] auto hash_to_hex(Hash256 const& hash) -> std::string;

/// Parse hex string to hash
[[nodiscard]] auto hex_to_hash(std::string_view hex) -> Result<Hash256>;

/// Check if two hashes are equal
[[nodiscard]] auto hash_equal(Hash256 const& a, Hash256 const& b) -> bool;

/// Zero hash constant
inline constexpr auto ZERO_HASH = Hash256 {};

/// Compute Merkle hash for a directory from sorted children entries
/// Each entry is (name, type, hash_ptr) - sorted by name before hashing
[[nodiscard]] auto compute_merkle_hash(
    std::vector<std::tuple<std::string_view, NodeType, Hash256 const*>> const& children
) -> Hash256;

} // namespace pup
