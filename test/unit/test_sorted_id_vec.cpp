// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/sorted_id_vec.hpp"

#include <vector>

// --- SortedIdVec ---

TEST_CASE("SortedIdVec empty state", "[sorted_id_vec]")
{
    auto vec = pup::SortedIdVec {};
    REQUIRE(vec.size() == 0);
    REQUIRE_FALSE(vec.contains(0));
    REQUIRE_FALSE(vec.contains(42));
    REQUIRE(vec.data() == nullptr);
}

TEST_CASE("SortedIdVec insert maintains sorted order", "[sorted_id_vec]")
{
    auto vec = pup::SortedIdVec {};
    REQUIRE(vec.insert(30));
    REQUIRE(vec.insert(10));
    REQUIRE(vec.insert(20));

    REQUIRE(vec.size() == 3);
    REQUIRE(vec.data()[0] == 10);
    REQUIRE(vec.data()[1] == 20);
    REQUIRE(vec.data()[2] == 30);
}

TEST_CASE("SortedIdVec duplicate insert is idempotent", "[sorted_id_vec]")
{
    auto vec = pup::SortedIdVec {};
    REQUIRE(vec.insert(5));
    REQUIRE_FALSE(vec.insert(5));
    REQUIRE(vec.size() == 1);
}

TEST_CASE("SortedIdVec contains", "[sorted_id_vec]")
{
    auto vec = pup::SortedIdVec {};
    vec.insert(1);
    vec.insert(100);
    vec.insert(50);

    REQUIRE(vec.contains(1));
    REQUIRE(vec.contains(50));
    REQUIRE(vec.contains(100));
    REQUIRE_FALSE(vec.contains(0));
    REQUIRE_FALSE(vec.contains(51));
    REQUIRE_FALSE(vec.contains(200));
}

TEST_CASE("SortedIdVec remove", "[sorted_id_vec]")
{
    auto vec = pup::SortedIdVec {};
    vec.insert(10);
    vec.insert(20);
    vec.insert(30);

    SECTION("remove existing returns true")
    {
        REQUIRE(vec.remove(20));
        REQUIRE(vec.size() == 2);
        REQUIRE_FALSE(vec.contains(20));
        REQUIRE(vec.contains(10));
        REQUIRE(vec.contains(30));
    }

    SECTION("remove non-existing returns false")
    {
        REQUIRE_FALSE(vec.remove(99));
        REQUIRE(vec.size() == 3);
    }

    SECTION("remove first element")
    {
        REQUIRE(vec.remove(10));
        REQUIRE(vec.size() == 2);
        REQUIRE(vec.data()[0] == 20);
    }

    SECTION("remove last element")
    {
        REQUIRE(vec.remove(30));
        REQUIRE(vec.size() == 2);
        REQUIRE(vec.data()[1] == 20);
    }
}

TEST_CASE("SortedIdVec clear", "[sorted_id_vec]")
{
    auto vec = pup::SortedIdVec {};
    vec.insert(1);
    vec.insert(2);
    vec.insert(3);

    vec.clear();
    REQUIRE(vec.size() == 0);
    REQUIRE_FALSE(vec.contains(1));
}

TEST_CASE("SortedIdVec for_each", "[sorted_id_vec]")
{
    auto vec = pup::SortedIdVec {};
    vec.insert(50);
    vec.insert(10);
    vec.insert(30);

    auto collected = std::vector<std::uint32_t> {};
    vec.for_each(
        [](std::uint32_t id, void* ctx) {
            static_cast<std::vector<std::uint32_t>*>(ctx)->push_back(id);
        },
        &collected
    );

    REQUIRE(collected == std::vector<std::uint32_t> { 10, 30, 50 });
}

TEST_CASE("SortedIdVec move semantics", "[sorted_id_vec]")
{
    auto a = pup::SortedIdVec {};
    a.insert(1);
    a.insert(2);

    SECTION("move constructor")
    {
        auto b = std::move(a);
        REQUIRE(b.size() == 2);
        REQUIRE(b.contains(1));
        REQUIRE(b.contains(2));
    }

    SECTION("move assignment")
    {
        auto b = pup::SortedIdVec {};
        b.insert(99);
        b = std::move(a);
        REQUIRE(b.size() == 2);
        REQUIRE(b.contains(1));
        REQUIRE_FALSE(b.contains(99));
    }
}

// --- SortedPairVec ---

TEST_CASE("SortedPairVec empty state", "[sorted_pair_vec]")
{
    auto vec = pup::SortedPairVec {};
    REQUIRE(vec.size() == 0);
    REQUIRE_FALSE(vec.contains(0));
    REQUIRE(vec.find(42) == nullptr);
}

TEST_CASE("SortedPairVec insert and find", "[sorted_pair_vec]")
{
    auto vec = pup::SortedPairVec {};
    REQUIRE(vec.insert(30, 300));
    REQUIRE(vec.insert(10, 100));
    REQUIRE(vec.insert(20, 200));

    REQUIRE(vec.size() == 3);

    auto const* v10 = vec.find(10);
    auto const* v20 = vec.find(20);
    auto const* v30 = vec.find(30);

    REQUIRE(v10 != nullptr);
    REQUIRE(*v10 == 100);
    REQUIRE(v20 != nullptr);
    REQUIRE(*v20 == 200);
    REQUIRE(v30 != nullptr);
    REQUIRE(*v30 == 300);
}

TEST_CASE("SortedPairVec overwrite existing key", "[sorted_pair_vec]")
{
    auto vec = pup::SortedPairVec {};
    REQUIRE(vec.insert(5, 50));
    REQUIRE_FALSE(vec.insert(5, 99));

    REQUIRE(vec.size() == 1);
    REQUIRE(*vec.find(5) == 99);
}

TEST_CASE("SortedPairVec contains", "[sorted_pair_vec]")
{
    auto vec = pup::SortedPairVec {};
    vec.insert(10, 1);
    vec.insert(20, 2);

    REQUIRE(vec.contains(10));
    REQUIRE(vec.contains(20));
    REQUIRE_FALSE(vec.contains(15));
}

TEST_CASE("SortedPairVec remove", "[sorted_pair_vec]")
{
    auto vec = pup::SortedPairVec {};
    vec.insert(10, 1);
    vec.insert(20, 2);
    vec.insert(30, 3);

    SECTION("remove existing returns true")
    {
        REQUIRE(vec.remove(20));
        REQUIRE(vec.size() == 2);
        REQUIRE_FALSE(vec.contains(20));
        REQUIRE(vec.contains(10));
        REQUIRE(vec.contains(30));
    }

    SECTION("remove non-existing returns false")
    {
        REQUIRE_FALSE(vec.remove(99));
        REQUIRE(vec.size() == 3);
    }
}

TEST_CASE("SortedPairVec for_each iterates in key order", "[sorted_pair_vec]")
{
    auto vec = pup::SortedPairVec {};
    vec.insert(50, 5);
    vec.insert(10, 1);
    vec.insert(30, 3);

    auto collected = std::vector<std::pair<std::uint32_t, std::uint32_t>> {};
    vec.for_each(
        [](std::uint32_t key, std::uint32_t value, void* ctx) {
            static_cast<std::vector<std::pair<std::uint32_t, std::uint32_t>>*>(ctx)->emplace_back(key, value);
        },
        &collected
    );

    REQUIRE(collected.size() == 3);
    REQUIRE(collected[0] == std::pair<std::uint32_t, std::uint32_t> { 10, 1 });
    REQUIRE(collected[1] == std::pair<std::uint32_t, std::uint32_t> { 30, 3 });
    REQUIRE(collected[2] == std::pair<std::uint32_t, std::uint32_t> { 50, 5 });
}

TEST_CASE("SortedPairVec clear", "[sorted_pair_vec]")
{
    auto vec = pup::SortedPairVec {};
    vec.insert(1, 10);
    vec.insert(2, 20);

    vec.clear();
    REQUIRE(vec.size() == 0);
    REQUIRE_FALSE(vec.contains(1));
    REQUIRE(vec.find(1) == nullptr);
}

TEST_CASE("SortedPairVec move semantics", "[sorted_pair_vec]")
{
    auto a = pup::SortedPairVec {};
    a.insert(1, 10);
    a.insert(2, 20);

    SECTION("move constructor")
    {
        auto b = std::move(a);
        REQUIRE(b.size() == 2);
        REQUIRE(*b.find(1) == 10);
        REQUIRE(*b.find(2) == 20);
    }

    SECTION("move assignment")
    {
        auto b = pup::SortedPairVec {};
        b.insert(99, 99);
        b = std::move(a);
        REQUIRE(b.size() == 2);
        REQUIRE(b.contains(1));
        REQUIRE_FALSE(b.contains(99));
    }
}
