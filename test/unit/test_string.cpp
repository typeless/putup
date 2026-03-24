// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/string.hpp"

using pup::String;

// =============================================================================
// Construction & basic accessors
// =============================================================================

TEST_CASE("String default construction", "[string]")
{
    auto s = String {};
    REQUIRE(s.empty());
    REQUIRE(s.size() == 0);
    REQUIRE(s.data() != nullptr);
    REQUIRE(s.c_str()[0] == '\0');
}

TEST_CASE("String from string_view", "[string]")
{
    auto s = String { "hello" };
    REQUIRE(s.size() == 5);
    REQUIRE(std::string_view { s } == "hello");
}

TEST_CASE("String from nullptr-safe c_str", "[string]")
{
    auto s = String { "test" };
    REQUIRE(s.c_str()[4] == '\0');
}

// =============================================================================
// SSO boundary
// =============================================================================

TEST_CASE("String SSO boundary", "[string]")
{
    SECTION("exactly SSO_CAP characters stay inline")
    {
        auto s = String { "1234567890123456789012" }; // 22 chars = SSO_CAP
        REQUIRE(s.size() == 22);
        REQUIRE(std::string_view { s } == "1234567890123456789012");
    }

    SECTION("SSO_CAP + 1 goes to heap")
    {
        auto s = String { "12345678901234567890123" }; // 23 chars > SSO_CAP
        REQUIRE(s.size() == 23);
        REQUIRE(std::string_view { s } == "12345678901234567890123");
    }
}

// =============================================================================
// Copy & move
// =============================================================================

TEST_CASE("String copy", "[string]")
{
    SECTION("copy short string (SSO)")
    {
        auto a = String { "hi" };
        auto b = String { a };
        REQUIRE(std::string_view { b } == "hi");
        // Modifying b doesn't affect a
        b += "!";
        REQUIRE(std::string_view { a } == "hi");
        REQUIRE(std::string_view { b } == "hi!");
    }

    SECTION("copy long string (heap)")
    {
        auto a = String { "this is a long string that exceeds SSO" };
        auto b = String { a };
        REQUIRE(std::string_view { b } == "this is a long string that exceeds SSO");
    }
}

TEST_CASE("String move", "[string]")
{
    auto a = String { "movable" };
    auto b = String { std::move(a) };
    REQUIRE(std::string_view { b } == "movable");
    REQUIRE(a.empty()); // moved-from is empty
}

// =============================================================================
// Mutation
// =============================================================================

TEST_CASE("String append", "[string]")
{
    SECTION("append to empty")
    {
        auto s = String {};
        s += "world";
        REQUIRE(std::string_view { s } == "world");
    }

    SECTION("append staying within SSO")
    {
        auto s = String { "hi" };
        s += " there";
        REQUIRE(std::string_view { s } == "hi there");
    }

    SECTION("append crossing SSO boundary")
    {
        auto s = String { "twelve_chars" }; // 12 chars
        s += "_plus_enough_to_overflow"; // pushes past 22
        REQUIRE(std::string_view { s } == "twelve_chars_plus_enough_to_overflow");
    }

    SECTION("append char")
    {
        auto s = String { "ab" };
        s += 'c';
        REQUIRE(std::string_view { s } == "abc");
    }
}

TEST_CASE("String reserve", "[string]")
{
    auto s = String {};
    s.reserve(100);
    s += "after reserve";
    REQUIRE(std::string_view { s } == "after reserve");
}

TEST_CASE("String clear", "[string]")
{
    auto s = String { "hello" };
    s.clear();
    REQUIRE(s.empty());
    REQUIRE(s.size() == 0);
}

// =============================================================================
// Searching
// =============================================================================

TEST_CASE("String find", "[string]")
{
    auto s = String { "hello/world/foo" };
    REQUIRE(s.find('/') == 5);
    REQUIRE(s.find('/', 6) == 11);
    REQUIRE(s.find('z') == String::npos);
    REQUIRE(s.find("world") == 6);
}

TEST_CASE("String rfind", "[string]")
{
    auto s = String { "hello/world/foo" };
    REQUIRE(s.rfind('/') == 11);
}

TEST_CASE("String starts_with / ends_with", "[string]")
{
    auto s = String { "CONFIG_DEBUG" };
    REQUIRE(s.starts_with("CONFIG_"));
    REQUIRE(s.ends_with("DEBUG"));
    REQUIRE_FALSE(s.starts_with("NOPE"));
    REQUIRE_FALSE(s.ends_with("NOPE"));
}

// =============================================================================
// substr
// =============================================================================

TEST_CASE("String substr", "[string]")
{
    auto s = String { "hello world" };
    auto sub = s.substr(6);
    REQUIRE(std::string_view { sub } == "world");

    auto sub2 = s.substr(0, 5);
    REQUIRE(std::string_view { sub2 } == "hello");
}

// =============================================================================
// Interop with std::string_view
// =============================================================================

TEST_CASE("String implicit conversion to string_view", "[string]")
{
    auto s = String { "test" };
    std::string_view sv = s;
    REQUIRE(sv == "test");
}

// =============================================================================
// Comparison operators
// =============================================================================

TEST_CASE("String comparison", "[string]")
{
    auto a = String { "abc" };
    REQUIRE(std::string_view { a } == "abc");
    REQUIRE(std::string_view { a } != "def");
    REQUIRE(std::string_view { a } < "def");
}

// =============================================================================
// operator[]
// =============================================================================

TEST_CASE("String element access", "[string]")
{
    auto s = String { "abc" };
    REQUIRE(s[0] == 'a');
    REQUIRE(s[2] == 'c');
    REQUIRE(s.back() == 'c');
}

// =============================================================================
// Copy assignment
// =============================================================================

TEST_CASE("String copy assignment", "[string]")
{
    auto a = String { "original" };
    auto b = String { "replaced" };
    a = b;
    REQUIRE(std::string_view { a } == "replaced");
    // b is unchanged
    REQUIRE(std::string_view { b } == "replaced");
}

TEST_CASE("String move assignment", "[string]")
{
    auto a = String { "original" };
    auto b = String { "replaced" };
    a = std::move(b);
    REQUIRE(std::string_view { a } == "replaced");
    REQUIRE(b.empty());
}

// =============================================================================
// Self-referential operations
// =============================================================================

TEST_CASE("String self-append", "[string]")
{
    SECTION("self-append within SSO")
    {
        auto s = String { "ab" };
        s += s;
        REQUIRE(s == "abab");
    }

    SECTION("self-append crossing SSO boundary")
    {
        auto s = String { "12345678901" }; // 11 chars, SSO
        s += s; // 22 chars = SSO_CAP, still SSO
        REQUIRE(s.size() == 22);
        REQUIRE(s == "1234567890112345678901");
    }

    SECTION("self-append SSO to heap")
    {
        auto s = String { "123456789012" }; // 12 chars
        s += s; // 24 chars > SSO_CAP, triggers SSO→heap
        REQUIRE(s.size() == 24);
        REQUIRE(s == "123456789012123456789012");
    }

    SECTION("self-append on heap with realloc")
    {
        auto s = String { "this is a long string that is on the heap" };
        auto expected = std::string { s.data(), s.size() };
        expected += expected;
        s += s;
        REQUIRE(std::string_view { s } == expected);
    }
}

TEST_CASE("String self-assignment", "[string]")
{
    auto s = String { "test" };
    s = s; // NOLINT(clang-diagnostic-self-assign-overloaded)
    REQUIRE(s == "test");
}

// =============================================================================
// Large strings
// =============================================================================

TEST_CASE("String large string", "[string]")
{
    auto s = String {};
    for (int i = 0; i < 10000; ++i) {
        s += 'x';
    }
    REQUIRE(s.size() == 10000);
    REQUIRE(s[0] == 'x');
    REQUIRE(s[9999] == 'x');
}

// =============================================================================
// Edge cases
// =============================================================================

TEST_CASE("String from empty literal", "[string]")
{
    auto s = String { "" };
    REQUIRE(s.empty());
    REQUIRE(s.size() == 0);
}

TEST_CASE("String move heap-allocated", "[string]")
{
    auto a = String { "this is a long heap-allocated string that exceeds SSO" };
    auto expected = std::string_view { a };
    auto b = String { std::move(a) };
    REQUIRE(std::string_view { b } == expected);
    REQUIRE(a.empty());
}

TEST_CASE("String heap growth via append", "[string]")
{
    auto s = String { "start with a heap string that is long enough" };
    auto original = std::string { s.data(), s.size() };
    for (int i = 0; i < 100; ++i) {
        s += " more";
    }
    REQUIRE(s.starts_with(original));
    REQUIRE(s.size() == original.size() + 500);
}

TEST_CASE("String substr out of range", "[string]")
{
    auto s = String { "hello" };
    auto sub = s.substr(100);
    REQUIRE(sub.empty());
}

TEST_CASE("String resize", "[string]")
{
    SECTION("grow")
    {
        auto s = String { "abc" };
        s.resize(10);
        REQUIRE(s.size() == 10);
        REQUIRE(s[0] == 'a');
        REQUIRE(s[3] == '\0');
    }

    SECTION("shrink")
    {
        auto s = String { "hello world" };
        s.resize(5);
        REQUIRE(s == "hello");
    }
}

TEST_CASE("String comparison operators", "[string]")
{
    auto a = String { "abc" };
    auto b = String { "def" };
    REQUIRE(a == "abc");
    REQUIRE(a == a);
    REQUIRE(a < b);
    REQUIRE_FALSE(a == b);
}
