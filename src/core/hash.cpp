// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/core/hash.hpp"
#include "pup/core/metrics.hpp"

extern "C" {
#include "sha256/sha256.h"
}

#include <algorithm>
#include <cstring>
#include <fstream>
#include <ranges>

namespace pup {

struct Sha256::Impl {
    sha256_ctx ctx;
};

Sha256::Sha256()
    : impl_(new Impl)
{
    sha256_init(&impl_->ctx);
}

Sha256::~Sha256()
{
    delete impl_;
}

Sha256::Sha256(Sha256&& other) noexcept
    : impl_(other.impl_)
{
    other.impl_ = nullptr;
}

auto Sha256::operator=(Sha256&& other) noexcept -> Sha256&
{
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

auto Sha256::update(std::span<std::byte const> data) -> void
{
    sha256_update(&impl_->ctx, data.data(), data.size());
}

auto Sha256::update(std::string_view data) -> void
{
    sha256_update(&impl_->ctx, data.data(), data.size());
}

auto Sha256::finalize() -> Hash256
{
    auto result = Hash256 {};
    sha256_final(&impl_->ctx, reinterpret_cast<uint8_t*>(result.data()));
    return result;
}

auto Sha256::reset() -> void
{
    sha256_init(&impl_->ctx);
}

auto sha256(std::span<std::byte const> data) -> Hash256
{
    auto hasher = Sha256 {};
    hasher.update(data);
    return hasher.finalize();
}

auto sha256(std::string_view data) -> Hash256
{
    auto hasher = Sha256 {};
    hasher.update(data);
    return hasher.finalize();
}

auto sha256_file(std::filesystem::path const& path) -> Result<Hash256>
{
    ++thread_metrics().hash_computations;

    auto file = std::ifstream { path, std::ios::binary };
    if (!file) {
        return make_error<Hash256>(ErrorCode::IoError, "Failed to open file: " + path.string());
    }

    auto hasher = Sha256 {};
    auto buffer = std::array<char, 8192> {};

    while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0) {
        auto const bytes_read = static_cast<std::size_t>(file.gcount());
        hasher.update(std::string_view { buffer.data(), bytes_read });
    }

    if (file.bad()) {
        return make_error<Hash256>(ErrorCode::IoError, "Error reading file: " + path.string());
    }

    return hasher.finalize();
}

auto hash_to_hex(Hash256 const& hash) -> std::string
{
    static constexpr auto hex_chars = std::string_view { "0123456789abcdef" };
    auto result = std::string {};
    result.reserve(hash.size() * 2);

    for (auto const byte : hash) {
        auto const b = static_cast<unsigned char>(byte);
        result.push_back(hex_chars[b >> 4]);
        result.push_back(hex_chars[b & 0x0F]);
    }

    return result;
}

auto hex_to_hash(std::string_view hex) -> Result<Hash256>
{
    if (hex.size() != 64) {
        return make_error<Hash256>(ErrorCode::InvalidArgument, "Hex string must be 64 characters");
    }

    auto result = Hash256 {};

    for (auto i = std::size_t { 0 }; i < 32; ++i) {
        auto const hi = char { hex[i * 2] };
        auto const lo = char { hex[i * 2 + 1] };

        auto parse_nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f') {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F') {
                return c - 'A' + 10;
            }
            return -1;
        };

        auto const hi_val = int { parse_nibble(hi) };
        auto const lo_val = int { parse_nibble(lo) };

        if (hi_val < 0 || lo_val < 0) {
            return make_error<Hash256>(ErrorCode::InvalidArgument, "Invalid hex character");
        }

        result[i] = static_cast<std::byte>((hi_val << 4) | lo_val);
    }

    return result;
}

auto hash_equal(Hash256 const& a, Hash256 const& b) -> bool
{
    return std::memcmp(a.data(), b.data(), a.size()) == 0;
}

auto compute_merkle_hash(
    std::vector<std::tuple<std::string_view, NodeType, Hash256 const*>> const& children) -> Hash256
{
    // Sort children by name for deterministic ordering
    auto sorted = children;
    std::ranges::sort(sorted, {}, [](auto const& x) { return std::get<0>(x); });

    // Compute hash from serialized children entries
    // Format: [name_length: 4 bytes BE][name: UTF-8][type: 1 byte][hash: 32 bytes]
    auto hasher = Sha256 {};

    for (auto const& [name, type, hash_ptr] : sorted) {
        // Name length (4 bytes, big-endian)
        auto const name_len = static_cast<std::uint32_t>(name.size());
        auto len_bytes = std::array<std::byte, 4> {
            static_cast<std::byte>((name_len >> 24) & 0xFF),
            static_cast<std::byte>((name_len >> 16) & 0xFF),
            static_cast<std::byte>((name_len >> 8) & 0xFF),
            static_cast<std::byte>(name_len & 0xFF),
        };
        hasher.update(std::span { len_bytes });

        // Name (UTF-8)
        hasher.update(name);

        // Type (1 byte)
        auto type_byte = std::array<std::byte, 1> { static_cast<std::byte>(type) };
        hasher.update(std::span { type_byte });

        // Hash (32 bytes) - use zero hash if null
        if (hash_ptr) {
            hasher.update(std::span { hash_ptr->data(), hash_ptr->size() });
        } else {
            hasher.update(std::span { ZERO_HASH.data(), ZERO_HASH.size() });
        }
    }

    return hasher.finalize();
}

} // namespace pup
