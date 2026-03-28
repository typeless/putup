// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/function.hpp"

using pup::Function;

TEST_CASE("Function basic operations", "[function]")
{
    SECTION("default is empty")
    {
        auto f = Function<void()> {};
        REQUIRE_FALSE(f);
    }

    SECTION("nullptr is empty")
    {
        auto f = Function<void()> { nullptr };
        REQUIRE_FALSE(f);
    }

    SECTION("lambda with no captures")
    {
        auto f = Function<int()> { [] { return 42; } };
        REQUIRE(f);
        REQUIRE(f() == 42);
    }

    SECTION("lambda with captures")
    {
        auto x = 10;
        auto f = Function<int(int)> { [&x](int y) { return x + y; } };
        REQUIRE(f(5) == 15);
        x = 20;
        REQUIRE(f(5) == 25);
    }

    SECTION("function pointer")
    {
        auto f = Function<int(int, int)> { [](int a, int b) -> int { return a * b; } };
        REQUIRE(f(6, 7) == 42);
    }

    SECTION("void return")
    {
        auto called = false;
        auto f = Function<void()> { [&called] { called = true; } };
        f();
        REQUIRE(called);
    }

    SECTION("assign nullptr clears")
    {
        auto f = Function<int()> { [] { return 1; } };
        REQUIRE(f);
        f = nullptr;
        REQUIRE_FALSE(f);
    }
}

TEST_CASE("Function move semantics", "[function]")
{
    SECTION("move construct")
    {
        auto f = Function<int()> { [] { return 42; } };
        auto g = std::move(f);
        REQUIRE(g);
        REQUIRE(g() == 42);
        REQUIRE_FALSE(f); // NOLINT(bugprone-use-after-move)
    }

    SECTION("move assign")
    {
        auto f = Function<int()> { [] { return 1; } };
        auto g = Function<int()> { [] { return 2; } };
        f = std::move(g);
        REQUIRE(f() == 2);
        REQUIRE_FALSE(g); // NOLINT(bugprone-use-after-move)
    }

    SECTION("move assign to empty")
    {
        auto f = Function<int()> {};
        auto g = Function<int()> { [] { return 42; } };
        f = std::move(g);
        REQUIRE(f() == 42);
    }

    SECTION("move assign from empty")
    {
        auto f = Function<int()> { [] { return 42; } };
        auto g = Function<int()> {};
        f = std::move(g);
        REQUIRE_FALSE(f);
    }
}

TEST_CASE("Function with large capture", "[function]")
{
    // Force heap allocation: 9 ints = 36 bytes > 32-byte SBO
    auto a = 1;
    auto b = 2;
    auto c = 3;
    auto d = 4;
    auto e = 5;
    auto g = 6;
    auto h = 7;
    auto i = 8;
    auto j = 9;
    auto f = Function<int()> { [a, b, c, d, e, g, h, i, j] { return a + b + c + d + e + g + h + i + j; } };
    REQUIRE(f() == 45);

    // Move the heap-allocated function
    auto moved = std::move(f);
    REQUIRE(moved() == 45);
    REQUIRE_FALSE(f); // NOLINT(bugprone-use-after-move)
}

TEST_CASE("Function with string_view args", "[function]")
{
    auto f = Function<bool(std::string_view)> { [](std::string_view s) { return s == "hello"; } };
    REQUIRE(f("hello"));
    REQUIRE_FALSE(f("world"));
}
