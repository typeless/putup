// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/cli/subcommand.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr auto LIST_SEP = ";";
constexpr auto EXE_SUFFIX = ".exe";
#else
constexpr auto LIST_SEP = ":";
constexpr auto EXE_SUFFIX = "";
#endif

auto found(std::string_view name, std::string const& search_path) -> std::string
{
    auto id = pup::cli::find_subcommand(name, search_path);
    return std::string { pup::global_pool().get(id) };
}

class TempTree {
public:
    explicit TempTree(std::string_view label)
        : m_root { fs::temp_directory_path() / ("pup_subcmd_" + std::string { label }) }
    {
        fs::remove_all(m_root);
        fs::create_directories(m_root);
    }

    ~TempTree() { fs::remove_all(m_root); }

    TempTree(TempTree const&) = delete;
    auto operator=(TempTree const&) -> TempTree& = delete;
    TempTree(TempTree&&) = delete;
    auto operator=(TempTree&&) -> TempTree& = delete;

    auto touch(std::string_view rel) const -> std::string
    {
        auto p = m_root / rel;
        fs::create_directories(p.parent_path());
        auto out = std::ofstream { p };
        out << "probe\n";
        return p.string();
    }

    auto mkdir(std::string_view rel) const -> void { fs::create_directories(m_root / rel); }

    [[nodiscard]] auto dir(std::string_view rel) const -> std::string
    {
        return (m_root / rel).string();
    }

    [[nodiscard]] auto root() const -> std::string { return m_root.string(); }

private:
    fs::path m_root;
};

} // namespace

TEST_CASE("find_subcommand resolves putup-<name> against a PATH list", "[subcommand]")
{
    SECTION("finds the prefixed executable in a search-path entry")
    {
        auto tree = TempTree { "hit" };
        auto expected = tree.touch(std::string { "bin/putup-demo" } + EXE_SUFFIX);

        REQUIRE(found("demo", tree.dir("bin")) == expected);
    }

    SECTION("returns empty when no entry holds it")
    {
        auto tree = TempTree { "miss" };
        tree.mkdir("bin");

        REQUIRE(found("demo", tree.dir("bin")).empty());
    }

    SECTION("scans entries left to right and takes the first hit")
    {
        auto tree = TempTree { "order" };
        auto first = tree.touch(std::string { "a/putup-demo" } + EXE_SUFFIX);
        tree.touch(std::string { "b/putup-demo" } + EXE_SUFFIX);

        REQUIRE(found("demo", tree.dir("a") + LIST_SEP + tree.dir("b")) == first);
    }

    SECTION("skips entries that do not hold it")
    {
        auto tree = TempTree { "skip" };
        tree.mkdir("a");
        auto second = tree.touch(std::string { "b/putup-demo" } + EXE_SUFFIX);

        REQUIRE(found("demo", tree.dir("a") + LIST_SEP + tree.dir("b")) == second);
    }

    SECTION("ignores an empty search path")
    {
        REQUIRE(found("demo", "").empty());
    }

    SECTION("ignores empty entries in the search path")
    {
        auto tree = TempTree { "empty_entry" };
        auto hit = tree.touch(std::string { "bin/putup-demo" } + EXE_SUFFIX);

        REQUIRE(found("demo", std::string { LIST_SEP } + tree.dir("bin")) == hit);
    }

    SECTION("rejects a directory that carries the name")
    {
        auto tree = TempTree { "isdir" };
        tree.mkdir(std::string { "bin/putup-demo" } + EXE_SUFFIX);

        REQUIRE(found("demo", tree.dir("bin")).empty());
    }
}

TEST_CASE("find_subcommand only accepts bare executable names", "[subcommand]")
{
    SECTION("rejects a name that reaches into a subdirectory")
    {
        auto tree = TempTree { "traversal" };
        tree.touch(std::string { "bin/putup-sub/demo" } + EXE_SUFFIX);

        REQUIRE(found("sub/demo", tree.dir("bin")).empty());
    }

    SECTION("rejects a name that climbs out of the entry")
    {
        auto tree = TempTree { "climb" };
        tree.touch(std::string { "bin/putup-../escaped" } + EXE_SUFFIX);

        REQUIRE(found("../escaped", tree.dir("bin")).empty());
    }

    SECTION("rejects an empty name")
    {
        auto tree = TempTree { "emptyname" };
        tree.touch(std::string { "bin/putup-" } + EXE_SUFFIX);

        REQUIRE(found("", tree.dir("bin")).empty());
    }

    SECTION("accepts dashes, dots, digits and underscores")
    {
        auto tree = TempTree { "charset" };
        auto hit = tree.touch(std::string { "bin/putup-my_sub.2-x" } + EXE_SUFFIX);

        REQUIRE(found("my_sub.2-x", tree.dir("bin")) == hit);
    }
}

#ifndef _WIN32
TEST_CASE("find_subcommand rejects shell-significant names", "[subcommand]")
{
    SECTION("rejects a glob pattern even when a matching file exists")
    {
        auto tree = TempTree { "glob" };
        tree.touch("bin/putup-build-*");

        REQUIRE(found("build-*", tree.dir("bin")).empty());
    }

    SECTION("rejects a backslash-bearing name")
    {
        auto tree = TempTree { "backslash" };
        tree.touch("bin/putup-a\\b");

        REQUIRE(found("a\\b", tree.dir("bin")).empty());
    }
}
#endif
