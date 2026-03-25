// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/vec.hpp"
#include "pup/core/string.hpp"

using pup::Vec;
using pup::String;

// =============================================================================
// Trivially-copyable element (uint32_t)
// =============================================================================

TEST_CASE("Vec<uint32_t> basic operations", "[vec]")
{
    auto v = Vec<std::uint32_t> {};

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
    }

    SECTION("reserve does not change size")
    {
        v.reserve(100);
        REQUIRE(v.empty());
        REQUIRE(v.capacity() >= 100);
    }

    SECTION("clear resets size")
    {
        v.push_back(1);
        v.push_back(2);
        v.clear();
        REQUIRE(v.empty());
    }

    SECTION("pop_back")
    {
        v.push_back(1);
        v.push_back(2);
        v.pop_back();
        REQUIRE(v.size() == 1);
        REQUIRE(v[0] == 1);
    }

    SECTION("front and back")
    {
        v.push_back(10);
        v.push_back(20);
        REQUIRE(v.front() == 10);
        REQUIRE(v.back() == 20);
    }

    SECTION("range-for iteration")
    {
        v.push_back(1);
        v.push_back(2);
        v.push_back(3);
        auto sum = std::uint32_t { 0 };
        for (auto x : v) {
            sum += x;
        }
        REQUIRE(sum == 6);
    }

    SECTION("resize grows with zero-init")
    {
        v.push_back(42);
        v.resize(5);
        REQUIRE(v.size() == 5);
        REQUIRE(v[0] == 42);
        REQUIRE(v[4] == 0);
    }

    SECTION("resize shrinks")
    {
        v.push_back(1);
        v.push_back(2);
        v.push_back(3);
        v.resize(1);
        REQUIRE(v.size() == 1);
        REQUIRE(v[0] == 1);
    }
}

// =============================================================================
// Copy and move (trivial)
// =============================================================================

TEST_CASE("Vec<uint32_t> copy", "[vec]")
{
    auto a = Vec<std::uint32_t> {};
    a.push_back(1);
    a.push_back(2);
    a.push_back(3);

    auto b = Vec<std::uint32_t> { a };
    REQUIRE(b.size() == 3);
    REQUIRE(b[0] == 1);

    b.push_back(4);
    REQUIRE(a.size() == 3); // independent
}

TEST_CASE("Vec<uint32_t> move", "[vec]")
{
    auto a = Vec<std::uint32_t> {};
    a.push_back(10);
    a.push_back(20);

    auto b = Vec<std::uint32_t> { std::move(a) };
    REQUIRE(b.size() == 2);
    REQUIRE(b[0] == 10);
    REQUIRE(a.empty());
}

// =============================================================================
// Non-trivial element (pup::String)
// =============================================================================

TEST_CASE("Vec<String> basic operations", "[vec]")
{
    auto v = Vec<String> {};

    SECTION("push_back and access")
    {
        v.push_back(String { "hello" });
        v.push_back(String { "world" });
        REQUIRE(v.size() == 2);
        REQUIRE(v[0] == "hello");
        REQUIRE(v[1] == "world");
    }

    SECTION("clear destroys elements")
    {
        v.push_back(String { "a long string that goes on the heap for sure" });
        v.clear();
        REQUIRE(v.empty());
    }

    SECTION("move element in")
    {
        auto s = String { "movable" };
        v.push_back(std::move(s));
        REQUIRE(v[0] == "movable");
        REQUIRE(s.empty());
    }
}

TEST_CASE("Vec<String> copy", "[vec]")
{
    auto a = Vec<String> {};
    a.push_back(String { "one" });
    a.push_back(String { "two" });

    auto b = Vec<String> { a };
    REQUIRE(b.size() == 2);
    REQUIRE(b[0] == "one");

    b[0] = String { "modified" };
    REQUIRE(a[0] == "one"); // independent
}

TEST_CASE("Vec<String> move", "[vec]")
{
    auto a = Vec<String> {};
    a.push_back(String { "hello" });

    auto b = Vec<String> { std::move(a) };
    REQUIRE(b[0] == "hello");
    REQUIRE(a.empty());
}

TEST_CASE("Vec<String> growth moves elements", "[vec]")
{
    auto v = Vec<String> {};
    for (int i = 0; i < 100; ++i) {
        v.push_back(String { "item" });
    }
    REQUIRE(v.size() == 100);
    REQUIRE(v[99] == "item");
}

// =============================================================================
// Insert and erase
// =============================================================================

TEST_CASE("Vec insert", "[vec]")
{
    auto v = Vec<std::uint32_t> {};
    v.push_back(1);
    v.push_back(3);
    v.insert(v.begin() + 1, 2);
    REQUIRE(v.size() == 3);
    REQUIRE(v[0] == 1);
    REQUIRE(v[1] == 2);
    REQUIRE(v[2] == 3);
}

TEST_CASE("Vec erase", "[vec]")
{
    auto v = Vec<std::uint32_t> {};
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    v.erase(v.begin() + 1);
    REQUIRE(v.size() == 2);
    REQUIRE(v[0] == 1);
    REQUIRE(v[1] == 3);
}

TEST_CASE("Vec<String> insert", "[vec]")
{
    auto v = Vec<String> {};
    v.push_back(String { "a" });
    v.push_back(String { "c" });
    v.insert(v.begin() + 1, String { "b" });
    REQUIRE(v.size() == 3);
    REQUIRE(v[0] == "a");
    REQUIRE(v[1] == "b");
    REQUIRE(v[2] == "c");
}

TEST_CASE("Vec<String> erase", "[vec]")
{
    auto v = Vec<String> {};
    v.push_back(String { "a" });
    v.push_back(String { "b" });
    v.push_back(String { "c" });
    v.erase(v.begin() + 1);
    REQUIRE(v.size() == 2);
    REQUIRE(v[0] == "a");
    REQUIRE(v[1] == "c");
}

TEST_CASE("Vec copy assignment", "[vec]")
{
    auto a = Vec<String> {};
    a.push_back(String { "old" });

    auto b = Vec<String> {};
    b.push_back(String { "one" });
    b.push_back(String { "two" });

    a = b;
    REQUIRE(a.size() == 2);
    REQUIRE(a[0] == "one");
    REQUIRE(b[0] == "one"); // b unchanged
}

TEST_CASE("Vec move assignment", "[vec]")
{
    auto a = Vec<String> {};
    a.push_back(String { "old" });

    auto b = Vec<String> {};
    b.push_back(String { "new" });

    a = std::move(b);
    REQUIRE(a[0] == "new");
    REQUIRE(b.empty());
}

TEST_CASE("Vec self-copy assignment", "[vec]")
{
    auto v = Vec<std::uint32_t> {};
    v.push_back(42);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-assign-overloaded"
#endif
    v = v;
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    REQUIRE(v.size() == 1);
    REQUIRE(v[0] == 42);
}

// =============================================================================
// emplace_back
// =============================================================================

TEST_CASE("Vec emplace_back", "[vec]")
{
    auto v = Vec<String> {};
    v.emplace_back("constructed in place");
    REQUIRE(v[0] == "constructed in place");
}
