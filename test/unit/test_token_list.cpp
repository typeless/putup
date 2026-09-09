// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/token_list.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"

#include <catch_amalgamated.hpp>

#include <cstdint>

using pup::NodeId;
using pup::TokenList;
using pup::Vec;

namespace {

auto starts_of(TokenList<NodeId> const& list) -> Vec<std::uint32_t>
{
    return list.starts();
}

auto joined(TokenList<NodeId> const& list, std::uint32_t number) -> Vec<NodeId>
{
    auto out = Vec<NodeId> {};
    for (auto id : list.token(number)) {
        out.push_back(id);
    }
    return out;
}

} // namespace

TEST_CASE("A list built without tokens gives every operand its own number", "[token_list]")
{
    auto const list = TokenList<NodeId> { Vec<NodeId> { 7, 8, 9 } };

    REQUIRE(list.token_count() == 3);
    REQUIRE(joined(list, 1) == Vec<NodeId> { 7 });
    REQUIRE(joined(list, 3) == Vec<NodeId> { 9 });
    REQUIRE(joined(list, 4).empty());
}

TEST_CASE("An empty list has no tokens", "[token_list]")
{
    auto const list = TokenList<NodeId> {};

    REQUIRE(list.token_count() == 0);
    REQUIRE(list.empty());
    REQUIRE(joined(list, 1).empty());
}

TEST_CASE("A token owning several operands names all of them", "[token_list]")
{
    auto const list = TokenList<NodeId>::grouped(Vec<NodeId> { 4, 5, 6 }, Vec<std::uint32_t> { 2, 2, 3 }, 3);

    REQUIRE(list.token_count() == 3);
    REQUIRE(joined(list, 1).empty());
    REQUIRE(joined(list, 2) == Vec<NodeId> { 4, 5 });
    REQUIRE(joined(list, 3) == Vec<NodeId> { 6 });
}

TEST_CASE("A trailing token that owns nothing still holds its number", "[token_list]")
{
    auto const list = TokenList<NodeId>::grouped(Vec<NodeId> { 4 }, Vec<std::uint32_t> { 1 }, 3);

    REQUIRE(list.token_count() == 3);
    REQUIRE(joined(list, 1) == Vec<NodeId> { 4 });
    REQUIRE(joined(list, 2).empty());
    REQUIRE(joined(list, 3).empty());
    REQUIRE(starts_of(list) == Vec<std::uint32_t> { 0, 1, 1, 1 });
}

TEST_CASE("Dropping an operand leaves every survivor under the number it was written with", "[token_list]")
{
    auto list = TokenList<NodeId>::grouped(Vec<NodeId> { 4, 5, 6 }, Vec<std::uint32_t> { 1, 2, 3 }, 3);

    list.drop(1);

    REQUIRE(list.size() == 2);
    REQUIRE(joined(list, 1) == Vec<NodeId> { 4 });
    REQUIRE(joined(list, 2).empty());
    REQUIRE(joined(list, 3) == Vec<NodeId> { 6 });
}

TEST_CASE("A recorded boundary that does not span its operands is refused", "[token_list]")
{
    REQUIRE_FALSE(TokenList<NodeId>::from_starts(Vec<NodeId> { 1, 2 }, Vec<std::uint32_t> {}).has_value());
    REQUIRE_FALSE(TokenList<NodeId>::from_starts(Vec<NodeId> { 1, 2 }, Vec<std::uint32_t> { 1, 2 }).has_value());
    REQUIRE_FALSE(TokenList<NodeId>::from_starts(Vec<NodeId> { 1, 2 }, Vec<std::uint32_t> { 0, 1 }).has_value());
    REQUIRE_FALSE(TokenList<NodeId>::from_starts(Vec<NodeId> { 1, 2 }, Vec<std::uint32_t> { 0, 2, 1, 2 }).has_value());
    REQUIRE(TokenList<NodeId>::from_starts(Vec<NodeId> { 1, 2 }, Vec<std::uint32_t> { 0, 2, 2 }).has_value());
}

SCENARIO("A grouping survives being recorded and read back", "[token_list][property]")
{
    GIVEN("every grouping of up to four operands into up to four tokens")
    {
        WHEN("each is written as its prefix sum and read back")
        {
            THEN("the operands and every token of the result are the ones it was built from")
            {
                for (auto count = std::uint32_t { 0 }; count <= 4; ++count) {
                    for (auto token_count = count; token_count <= 4; ++token_count) {
                        for (auto shape = std::uint32_t { 0 }; shape < 256; ++shape) {
                            auto ids = Vec<NodeId> {};
                            auto token_of = Vec<std::uint32_t> {};
                            auto token = std::uint32_t { 1 };
                            auto legal = true;
                            for (auto i = std::uint32_t { 0 }; i < count; ++i) {
                                token += (shape >> (2 * i)) & 0x3U;
                                if (token > token_count) {
                                    legal = false;
                                    break;
                                }
                                ids.push_back(static_cast<NodeId>(i + 1));
                                token_of.push_back(token);
                            }
                            if (!legal) {
                                continue;
                            }

                            auto const built = TokenList<NodeId>::grouped(ids, token_of, token_count);
                            auto const read = TokenList<NodeId>::from_starts(built.ids(), built.starts());

                            INFO("count " << count << " tokens " << token_count << " shape " << shape);
                            REQUIRE(read.has_value());
                            REQUIRE(*read == built);
                            for (auto n = std::uint32_t { 1 }; n <= token_count; ++n) {
                                REQUIRE(joined(*read, n) == joined(built, n));
                            }
                        }
                    }
                }
            }
        }
    }
}
