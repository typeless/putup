// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"

#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/parser/regex.hpp"

#include <cstdint>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <regex.h>
#endif

using pup::parser::Regex;

namespace {

struct SplitMix64 {
    std::uint64_t state;

    auto next() -> std::uint64_t
    {
        auto z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    auto below(std::uint64_t bound) -> std::uint64_t { return next() % bound; }
};

auto random_string(SplitMix64& rng, std::string_view alphabet, std::size_t max_length) -> std::string
{
    auto text = std::string {};
    auto const length = rng.below(max_length + 1);
    for (auto i = std::uint64_t { 0 }; i < length; ++i) {
        text += alphabet[rng.below(alphabet.size())];
    }
    return text;
}

auto escape(std::string_view text) -> std::string
{
    auto escaped = std::string {};
    for (auto c : text) {
        auto const alphanumeric = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alphanumeric) {
            escaped += '\\';
        }
        escaped += c;
    }
    return escaped;
}

auto searches(std::string_view pattern, std::string_view subject) -> bool
{
    auto regex = Regex::compile(pattern);
    REQUIRE(regex.has_value());
    return regex->search(subject);
}

auto generate_alternation(SplitMix64& rng, int depth, bool anchored) -> std::string;

auto generate_atom(SplitMix64& rng, int depth) -> std::string
{
    constexpr auto classes = std::array<std::string_view, 5> { "[ab]", "[a-c]", "[^a]", "[./]", "[^./]" };
    constexpr auto literals = std::string_view { "abc/" };
    auto const choice = rng.below(depth > 0 ? 6 : 5);
    auto atom = std::string {};
    switch (choice) {
    case 0:
    case 1:
        atom = std::string(1, literals[rng.below(literals.size())]);
        break;
    case 2:
        atom = "\\.";
        break;
    case 3:
        atom = ".";
        break;
    case 4:
        atom = std::string { classes[rng.below(classes.size())] };
        break;
    default:
        atom = "(" + generate_alternation(rng, depth - 1, false) + ")";
        break;
    }
    constexpr auto quantifiers = std::array<std::string_view, 6> { "", "", "", "*", "+", "?" };
    return atom + std::string { quantifiers[rng.below(quantifiers.size())] };
}

auto generate_alternation(SplitMix64& rng, int depth, bool anchored) -> std::string
{
    auto pattern = std::string {};
    auto const branches = 1 + rng.below(2);
    for (auto b = std::uint64_t { 0 }; b < branches; ++b) {
        if (b > 0) {
            pattern += '|';
        }
        if (anchored && rng.below(4) == 0) {
            pattern += '^';
        }
        auto const atoms = 1 + rng.below(3);
        for (auto a = std::uint64_t { 0 }; a < atoms; ++a) {
            pattern += generate_atom(rng, depth);
        }
        if (anchored && rng.below(4) == 0) {
            pattern += '$';
        }
    }
    return pattern;
}

} // namespace

TEST_CASE("Regex searches the subject for the pattern's match", "[regex]")
{
    auto const [pattern, subject, expected] = GENERATE(table<char const*, char const*, bool>({
        { "b\\.c", "src/b.c", true },
        { "b\\.c", "src/bxc", false },
        { "b.c", "bxc", true },
        { "^b", "ab", false },
        { "^a", "ab", true },
        { "c$", "abc", true },
        { "b$", "abc", false },
        { "^$", "", true },
        { "", "anything", true },
        { "a[0-9]+b", "xa123by", true },
        { "a[0-9]+b", "xab", false },
        { "[^a-c]", "abc", false },
        { "[^a-c]", "abcd", true },
        { "[]a]", "]", true },
        { "[a-]", "-", true },
        { "[\\]]", "]", true },
        { "x\\d", "x7", true },
        { "x\\d", "xa", false },
        { "x\\D", "xa", true },
        { "\\w+\\.o$", "foo_1.o", true },
        { "^\\W", "_a", false },
        { "a\\sb", "a b", true },
        { "a\\Sb", "a b", false },
        { "[\\d.]+", ".", true },
        { "colou?r", "color", true },
        { "colou?r", "colouur", false },
        { "(ab)+c", "ababc", true },
        { "(ab)+c", "ac", false },
        { "^(foo|bar)\\.c$", "bar.c", true },
        { "^(foo|bar)\\.c$", "baz.c", false },
        { "a|", "zzz", true },
        { "(a*)*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac", false },
        { "\\(\\)\\[\\]\\{\\}\\*\\+\\?\\|\\^\\$\\\\\\/", "()[]{}*+?|^$\\/", true },
        { "a]}", "a]}", true },
    }));
    CAPTURE(pattern, subject);

    REQUIRE(searches(pattern, subject) == expected);
}

TEST_CASE("Regex refuses syntax outside its subset with the offset and the reason", "[regex]")
{
    auto const [pattern, offset, reason] = GENERATE(table<char const*, int, char const*>({
        { "a{2}", 1, "unsupported syntax '{'" },
        { "(?:a)", 1, "unsupported syntax '(?'" },
        { "(a)\\1", 4, "unsupported escape" },
        { "\\bfoo", 1, "unsupported escape" },
        { "x\\n", 2, "unsupported escape" },
        { "[[:alpha:]]", 1, "POSIX" },
        { "a*?", 2, "cannot follow a quantifier" },
        { "a++", 2, "cannot follow a quantifier" },
        { "*a", 0, "does not follow a repeatable item" },
        { "(*a)", 1, "does not follow a repeatable item" },
        { "a|+", 2, "does not follow a repeatable item" },
        { "^*", 1, "does not follow a repeatable item" },
        { "(a", 2, "missing )" },
        { "a)", 1, "unmatched )" },
        { "[ab", 3, "missing ]" },
        { "[z-a]", 3, "range out of order" },
        { "[\\d-z]", 3, "invalid range" },
        { "a\\", 2, "\\ at end of pattern" },
    }));
    CAPTURE(pattern);

    auto regex = Regex::compile(pattern);
    REQUIRE_FALSE(regex.has_value());
    auto const message = std::string { pup::global_pool().get(regex.error().message) };
    CAPTURE(message);
    REQUIRE(message.find(std::string { "regular expression '" } + pattern + "'") != std::string::npos);
    REQUIRE(message.find("at offset " + std::to_string(offset) + ":") != std::string::npos);
    REQUIRE(message.find(reason) != std::string::npos);
}

TEST_CASE("Regex of an escaped string finds exactly the subjects containing that string", "[regex][property]")
{
    constexpr auto alphabet = std::string_view { "ab.*+?()[]{}|^$\\/-" };
    auto rng = SplitMix64 { 0x5EEDu };
    for (auto i = 0; i < 3000; ++i) {
        auto const needle = random_string(rng, alphabet, 4);
        auto subject = random_string(rng, alphabet, 8);
        if (rng.below(2) == 0) {
            subject.insert(rng.below(subject.size() + 1), needle);
        }
        CAPTURE(needle, subject);

        REQUIRE(searches(escape(needle), subject) == (subject.find(needle) != std::string::npos));
    }
}

#ifndef _WIN32
TEST_CASE("Regex agrees with POSIX extended regexec on the subset both read alike", "[regex][property]")
{
    auto rng = SplitMix64 { 0xC0FFEEu };
    for (auto i = 0; i < 3000; ++i) {
        auto const pattern = generate_alternation(rng, 2, true);
        auto posix = regex_t {};
        REQUIRE(regcomp(&posix, pattern.c_str(), REG_EXTENDED | REG_NOSUB) == 0);
        for (auto s = 0; s < 8; ++s) {
            auto const subject = random_string(rng, "abc./", 8);
            CAPTURE(pattern, subject);

            REQUIRE(searches(pattern, subject) == (regexec(&posix, subject.c_str(), 0, nullptr, 0) == 0));
        }
        regfree(&posix);
    }
}
#endif
