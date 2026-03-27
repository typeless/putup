// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/paged_vec.hpp"
#include <string>

using pup::PagedVec;

TEST_CASE("PagedVec basic operations", "[paged_vec]")
{
    auto v = PagedVec<int, 4> {};

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

    SECTION("clear resets size")
    {
        v.push_back(1);
        v.push_back(2);
        v.clear();
        REQUIRE(v.empty());
    }
}

TEST_CASE("PagedVec pointer stability", "[paged_vec]")
{
    auto v = PagedVec<int, 4> {};

    v.push_back(42);
    auto* ptr = &v[0];

    // Fill first page and spill into second
    for (int i = 1; i < 10; ++i) {
        v.push_back(i);
    }

    // Original pointer still valid
    REQUIRE(*ptr == 42);
    REQUIRE(&v[0] == ptr);
}

TEST_CASE("PagedVec with non-trivial type", "[paged_vec]")
{
    auto v = PagedVec<std::string, 4> {};

    v.push_back(std::string { "hello" });
    v.push_back(std::string { "world" });

    auto* ptr = &v[0];

    // Push more to trigger new pages
    for (int i = 0; i < 20; ++i) {
        v.push_back(std::string { "item" });
    }

    // Original pointer still valid
    REQUIRE(*ptr == "hello");
    REQUIRE(v[1] == "world");
    REQUIRE(v.size() == 22);
}

TEST_CASE("PagedVec move", "[paged_vec]")
{
    auto a = PagedVec<int, 4> {};
    a.push_back(1);
    a.push_back(2);

    auto b = PagedVec<int, 4> { std::move(a) };
    REQUIRE(b.size() == 2);
    REQUIRE(b[0] == 1);
    REQUIRE(a.empty());
}

TEST_CASE("PagedVec emplace_back", "[paged_vec]")
{
    auto v = PagedVec<std::string, 4> {};
    auto& ref = v.emplace_back("constructed");
    REQUIRE(ref == "constructed");
    REQUIRE(v[0] == "constructed");
}

TEST_CASE("PagedVec large page crossing", "[paged_vec]")
{
    auto v = PagedVec<int, 4> {};
    for (int i = 0; i < 1000; ++i) {
        v.push_back(i);
    }
    REQUIRE(v.size() == 1000);
    REQUIRE(v[0] == 0);
    REQUIRE(v[999] == 999);
    REQUIRE(v[4] == 4);   // second page
    REQUIRE(v[8] == 8);   // third page
}
