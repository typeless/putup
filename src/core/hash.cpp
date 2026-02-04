// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/hash.hpp"
#include "pup/core/metrics.hpp"

extern "C" {
#include "sha256/sha256.h"
}

#include <cstring>
#include <fstream>

namespace pup {

namespace {

auto to_ctx(Sha256State const& state) -> sha256_ctx
{
    auto ctx = sha256_ctx {};
    std::memcpy(ctx.state, state.state.data(), sizeof(ctx.state));
    ctx.size = state.size;
    ctx.offset = state.offset;
    std::memcpy(ctx.buf, state.buffer.data(), sizeof(ctx.buf));
    return ctx;
}

auto from_ctx(sha256_ctx const& ctx) -> Sha256State
{
    auto state = Sha256State {};
    std::memcpy(state.state.data(), ctx.state, sizeof(ctx.state));
    state.size = ctx.size;
    state.offset = ctx.offset;
    std::memcpy(state.buffer.data(), ctx.buf, sizeof(ctx.buf));
    return state;
}

} // namespace

auto sha256_init() -> Sha256State
{
    auto ctx = sha256_ctx {};
    ::sha256_init(&ctx);
    return from_ctx(ctx);
}

auto sha256_update(Sha256State state, std::span<std::byte const> data) -> Sha256State
{
    auto ctx = to_ctx(state);
    ::sha256_update(&ctx, data.data(), data.size());
    return from_ctx(ctx);
}

auto sha256_update(Sha256State state, std::string_view data) -> Sha256State
{
    auto ctx = to_ctx(state);
    ::sha256_update(&ctx, data.data(), data.size());
    return from_ctx(ctx);
}

auto sha256_finalize(Sha256State state) -> Hash256
{
    auto ctx = to_ctx(state);
    auto result = Hash256 {};
    ::sha256_final(&ctx, reinterpret_cast<uint8_t*>(result.data()));
    return result;
}

auto sha256(std::span<std::byte const> data) -> Hash256
{
    auto state = sha256_init();
    state = sha256_update(state, data);
    return sha256_finalize(state);
}

auto sha256(std::string_view data) -> Hash256
{
    auto state = sha256_init();
    state = sha256_update(state, data);
    return sha256_finalize(state);
}

auto sha256_file(std::filesystem::path const& path) -> Result<Hash256>
{
    ++thread_metrics().hash_computations;

    auto file = std::ifstream { path, std::ios::binary };
    if (!file) {
        return make_error<Hash256>(ErrorCode::IoError, "Failed to open file: " + path.string());
    }

    auto state = sha256_init();
    auto buffer = std::array<char, 8192> {};

    while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0) {
        auto const bytes_read = static_cast<std::size_t>(file.gcount());
        state = sha256_update(state, std::string_view { buffer.data(), bytes_read });
    }

    if (file.bad()) {
        return make_error<Hash256>(ErrorCode::IoError, "Error reading file: " + path.string());
    }

    return sha256_finalize(state);
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

        auto const hi_val = parse_nibble(hi);
        auto const lo_val = parse_nibble(lo);

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

} // namespace pup
