// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "result.hpp"
#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace pup {

/// SHA-256 state for streaming hashes
/// Transparent struct - no PIMPL, suitable for stack allocation
struct Sha256State {
    std::array<std::uint32_t, 8> state;
    std::uint64_t size;
    std::uint32_t offset;
    std::array<std::uint8_t, 64> buffer;
};

/// Initialize SHA-256 state
[[nodiscard]]
auto sha256_init() -> Sha256State;

/// Update SHA-256 state with data (functional style - returns new state)
[[nodiscard]]
auto sha256_update(Sha256State state, std::span<std::byte const> data) -> Sha256State;

/// Update SHA-256 state with string data
[[nodiscard]]
auto sha256_update(Sha256State state, std::string_view data) -> Sha256State;

/// Finalize SHA-256 and return the hash
[[nodiscard]]
auto sha256_finalize(Sha256State state) -> Hash256;

/// Compute SHA-256 hash of a byte span
[[nodiscard]]
auto sha256(std::span<std::byte const> data) -> Hash256;

/// Compute SHA-256 hash of a string
[[nodiscard]]
auto sha256(std::string_view data) -> Hash256;

/// Compute SHA-256 hash of a file
[[nodiscard]]
auto sha256_file(std::string const& path) -> Result<Hash256>;

/// Convert hash to hex string (lowercase)
[[nodiscard]]
auto hash_to_hex(Hash256 const& hash) -> std::string;

/// Parse hex string to hash
[[nodiscard]]
auto hex_to_hash(std::string_view hex) -> Result<Hash256>;

/// Check if two hashes are equal
[[nodiscard]]
auto hash_equal(Hash256 const& a, Hash256 const& b) -> bool;

/// Zero hash constant
inline constexpr auto ZERO_HASH = Hash256 {};

} // namespace pup
