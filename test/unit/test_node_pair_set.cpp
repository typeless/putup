// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/node_pair_set.hpp"

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

using namespace pup;

namespace {

/// Deterministic sequence, so a failure reproduces from the reported seed.
class Lcg {
public:
    explicit Lcg(std::uint64_t seed)
        : state_ { seed }
    {
    }

    auto next(std::uint32_t bound) -> std::uint32_t
    {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state_ >> 33) % bound;
    }

private:
    std::uint64_t state_;
};

} // namespace

SCENARIO("NodeIdPairSet reports whether a pair is new", "[node_pair_set]")
{
    GIVEN("an empty set")
    {
        auto set = NodeIdPairSet {};

        WHEN("a pair is inserted twice")
        {
            auto const first = set.insert(NodeId { 1 }, NodeId { 2 });
            auto const second = set.insert(NodeId { 1 }, NodeId { 2 });

            THEN("only the first insertion is new")
            {
                REQUIRE(first);
                REQUIRE_FALSE(second);
            }

            THEN("the duplicate is not stored")
            {
                REQUIRE(set.size() == 1);
            }
        }

        WHEN("pairs differing only by order are inserted")
        {
            REQUIRE(set.insert(NodeId { 1 }, NodeId { 2 }));

            THEN("the reversed pair is a distinct member")
            {
                REQUIRE(set.insert(NodeId { 2 }, NodeId { 1 }));
                REQUIRE(set.size() == 2);
            }
        }

        WHEN("pairs sharing one component are inserted")
        {
            REQUIRE(set.insert(NodeId { 7 }, NodeId { 1 }));

            THEN("neither component alone decides membership")
            {
                REQUIRE(set.insert(NodeId { 7 }, NodeId { 2 }));
                REQUIRE(set.insert(NodeId { 8 }, NodeId { 1 }));
                REQUIRE(set.size() == 3);
            }
        }
    }
}

SCENARIO("NodeIdPairSet agrees with a reference set", "[node_pair_set]")
{
    GIVEN("a randomized sequence of pairs with many repeats")
    {
        auto const seed = GENERATE(1u, 2u, 3u, 12345u, 999983u);
        auto const bound = GENERATE(4u, 37u, 512u);

        auto rng = Lcg { seed };
        auto subject = NodeIdPairSet {};
        auto reference = std::set<std::pair<std::uint32_t, std::uint32_t>> {};

        auto subject_verdicts = std::vector<bool> {};
        auto reference_verdicts = std::vector<bool> {};

        WHEN("the same sequence is fed to both")
        {
            for (auto i = 0; i < 4000; ++i) {
                auto const from = rng.next(bound);
                auto const to = rng.next(bound);
                subject_verdicts.push_back(subject.insert(NodeId { from }, NodeId { to }));
                reference_verdicts.push_back(reference.insert({ from, to }).second);
            }

            THEN("every insertion returns the same verdict")
            {
                INFO("seed: " << seed << " bound: " << bound);
                REQUIRE(subject_verdicts == reference_verdicts);
            }

            THEN("both hold the same number of members")
            {
                INFO("seed: " << seed << " bound: " << bound);
                REQUIRE(subject.size() == reference.size());
            }
        }
    }
}

SCENARIO("NodeIdPairSet handles sparse ids without collisions", "[node_pair_set]")
{
    GIVEN("pairs drawn from a wide id range")
    {
        auto rng = Lcg { 4242 };
        auto subject = NodeIdPairSet {};
        auto reference = std::set<std::pair<std::uint32_t, std::uint32_t>> {};

        WHEN("many distinct pairs are inserted")
        {
            auto agreed = true;
            for (auto i = 0; i < 20000; ++i) {
                auto const from = rng.next(1u << 20);
                auto const to = rng.next(1u << 20);
                agreed = agreed
                    && subject.insert(NodeId { from }, NodeId { to }) == reference.insert({ from, to }).second;
            }

            THEN("no pair is wrongly reported as already present")
            {
                REQUIRE(agreed);
                REQUIRE(subject.size() == reference.size());
            }
        }
    }
}
