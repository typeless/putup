// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/hash.hpp"

TEST_CASE("SHA-256 hash computation", "[hash]")
{
    SECTION("empty string")
    {
        auto const hash = pup::sha256("");
        auto const hex = pup::hash_to_hex(hash);
        REQUIRE(hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    }

    SECTION("hello world")
    {
        auto const hash = pup::sha256("hello world");
        auto const hex = pup::hash_to_hex(hash);
        REQUIRE(hex == "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
    }

    SECTION("Hello, pup!")
    {
        auto const hash = pup::sha256("Hello, pup!");
        auto const hex = pup::hash_to_hex(hash);
        REQUIRE(hex == "7f46e18d14d4ae15927cb3cd6a2f831196e9a1bf4c4eb8ca362581cb741b9b54");
    }
}

TEST_CASE("Hash to hex conversion", "[hash]")
{
    SECTION("round-trip")
    {
        auto const original = pup::sha256("test");
        auto const hex = pup::hash_to_hex(original);
        auto const parsed = pup::hex_to_hash(hex);

        REQUIRE(parsed.has_value());
        REQUIRE(pup::hash_equal(original, *parsed));
    }

    SECTION("invalid hex length")
    {
        auto const result = pup::hex_to_hash("abc");
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == pup::ErrorCode::InvalidArgument);
    }

    SECTION("invalid hex character")
    {
        auto const result = pup::hex_to_hash(
            "gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg");
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == pup::ErrorCode::InvalidArgument);
    }
}

TEST_CASE("Streaming hash API", "[hash]")
{
    SECTION("update in chunks matches single call")
    {
        auto state = pup::sha256_init();
        state = pup::sha256_update(state, "hello ");
        state = pup::sha256_update(state, "world");
        auto const incremental = pup::sha256_finalize(state);

        auto const single = pup::sha256("hello world");

        REQUIRE(pup::hash_equal(incremental, single));
    }

    SECTION("fresh state for new hash")
    {
        auto state1 = pup::sha256_init();
        state1 = pup::sha256_update(state1, "first");
        (void)pup::sha256_finalize(state1);

        auto state2 = pup::sha256_init();
        state2 = pup::sha256_update(state2, "second");
        auto const result = pup::sha256_finalize(state2);

        auto const expected = pup::sha256("second");
        REQUIRE(pup::hash_equal(result, expected));
    }
}

TEST_CASE("Zero hash constant", "[hash]")
{
    auto const zero = pup::ZERO_HASH;
    for (auto const byte : zero) {
        REQUIRE(byte == std::byte{0});
    }
}
