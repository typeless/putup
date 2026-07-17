// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/stable_vec.hpp"

#include <string>
#include <utility>

using pup::StableVec;

namespace {

struct DtorCounter {
    int* counter;
    explicit DtorCounter(int* c)
        : counter(c)
    {
    }
    DtorCounter(DtorCounter&&) = default;
    ~DtorCounter()
    {
        if (counter) {
            ++*counter;
        }
    }
};

} // namespace

TEST_CASE("StableVec basic operations", "[stable_vec]")
{
    auto v = StableVec<int> {};

    SECTION("default is empty")
    {
        REQUIRE(v.empty());
        REQUIRE(v.size() == 0);
    }

    SECTION("push_back and access")
    {
        v.push_back(10);
        v.push_back(20);
        v.push_back(30);
        REQUIRE(v.size() == 3);
        REQUIRE(v[0] == 10);
        REQUIRE(v[1] == 20);
        REQUIRE(v[2] == 30);
        REQUIRE(v.back() == 30);
    }

    SECTION("clear resets size")
    {
        v.push_back(1);
        v.push_back(2);
        v.clear();
        REQUIRE(v.empty());
    }

    SECTION("resize default-constructs up to the requested size")
    {
        v.resize(5);
        REQUIRE(v.size() == 5);
        REQUIRE(v[4] == 0);
    }
}

TEST_CASE("StableVec pointer stability", "[stable_vec]")
{
    auto v = StableVec<int> {};

    v.push_back(42);
    auto* ptr = &v[0];

    for (int i = 1; i < 10000; ++i) {
        v.push_back(i);
    }

    REQUIRE(*ptr == 42);
    REQUIRE(&v[0] == ptr);
}

TEST_CASE("StableVec with non-trivial type", "[stable_vec]")
{
    auto v = StableVec<std::string> {};

    v.push_back(std::string { "hello" });
    v.push_back(std::string { "world" });

    auto* ptr = &v[0];

    for (int i = 0; i < 20; ++i) {
        v.push_back(std::string { "item" });
    }

    REQUIRE(*ptr == "hello");
    REQUIRE(v[1] == "world");
    REQUIRE(v.size() == 22);
}

TEST_CASE("StableVec runs element destructors", "[stable_vec]")
{
    auto destroyed = 0;

    SECTION("on destruction")
    {
        {
            auto v = StableVec<DtorCounter> {};
            v.emplace_back(&destroyed);
            v.emplace_back(&destroyed);
        }
        REQUIRE(destroyed == 2);
    }

    SECTION("on clear")
    {
        auto v = StableVec<DtorCounter> {};
        v.emplace_back(&destroyed);
        v.emplace_back(&destroyed);
        v.emplace_back(&destroyed);
        v.clear();
        REQUIRE(destroyed == 3);
    }
}

TEST_CASE("StableVec move", "[stable_vec]")
{
    auto a = StableVec<int> {};
    a.push_back(1);
    a.push_back(2);

    auto b = StableVec<int> { std::move(a) };
    REQUIRE(b.size() == 2);
    REQUIRE(b[0] == 1);
    REQUIRE(a.empty());

    auto c = StableVec<int> {};
    c.push_back(9);
    c = std::move(b);
    REQUIRE(c.size() == 2);
    REQUIRE(c[1] == 2);
}

TEST_CASE("StableVec emplace_back", "[stable_vec]")
{
    auto v = StableVec<std::string> {};
    auto& ref = v.emplace_back("constructed");
    REQUIRE(ref == "constructed");
    REQUIRE(v[0] == "constructed");
}

TEST_CASE("StableVec iteration", "[stable_vec]")
{
    auto v = StableVec<int> {};

    SECTION("empty range")
    {
        auto visited = 0;
        for (auto const& x : v) {
            (void)x;
            ++visited;
        }
        REQUIRE(visited == 0);
    }

    SECTION("visits all elements in order")
    {
        for (int i = 0; i < 1000; ++i) {
            v.push_back(i);
        }
        auto expected = 0;
        for (auto const& x : v) {
            REQUIRE(x == expected);
            ++expected;
        }
        REQUIRE(expected == 1000);
    }
}
