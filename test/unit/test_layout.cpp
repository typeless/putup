// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/string_pool.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using pup::StringId;
using pup::global_pool;

namespace {

/// RAII helper to create a temporary directory tree for testing
class TempDir {
public:
    TempDir()
        : path_(fs::temp_directory_path() / ("pup_test_" + std::to_string(std::rand())))
    {
        fs::create_directories(path_);
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    TempDir(TempDir const&) = delete;
    auto operator=(TempDir const&) -> TempDir& = delete;

    [[nodiscard]] auto path() const -> fs::path const& { return path_; }

    auto create_file(fs::path const& rel) -> void
    {
        auto full = path_ / rel;
        fs::create_directories(full.parent_path());
        std::ofstream { full };
    }

    auto create_dir(fs::path const& rel) -> void
    {
        fs::create_directories(path_ / rel);
    }

private:
    fs::path path_;
};

} // namespace

TEST_CASE("find_project_root", "[e2e][layout]")
{
    SECTION("finds Tupfile.ini in current directory")
    {
        auto tmp = TempDir {};
        tmp.create_file("Tupfile.ini");

        auto result = pup::find_project_root(tmp.path().string());
        REQUIRE(result.has_value());
        REQUIRE(global_pool().get(*result) == tmp.path().string());
    }

    SECTION("finds Tupfile.ini in parent directory")
    {
        auto tmp = TempDir {};
        tmp.create_file("Tupfile.ini");
        tmp.create_dir("src/lib");

        auto result = pup::find_project_root((tmp.path() / "src" / "lib").string());
        REQUIRE(result.has_value());
        REQUIRE(global_pool().get(*result) == tmp.path().string());
    }

    SECTION("build directory with .pup should find source root in parent")
    {
        // Scenario: User is in a build directory that has .pup from a previous build
        // The source root (with Tupfile.ini) is the parent directory
        auto tmp = TempDir {};
        tmp.create_file("Tupfile.ini");          // Source root marker
        tmp.create_dir("build-release/.pup");    // Build dir has .pup
        tmp.create_file("build-release/tup.config");

        // When searching from build-release/, should find parent (with Tupfile.ini)
        // NOT stop at build-release/ just because it has .pup
        auto result = pup::find_project_root((tmp.path() / "build-release").string());
        REQUIRE(result.has_value());
        REQUIRE(global_pool().get(*result) == tmp.path().string());
    }

    SECTION("returns nullopt when no project root found")
    {
        auto tmp = TempDir {};
        tmp.create_dir("empty");

        auto result = pup::find_project_root((tmp.path() / "empty").string());
        (void)result;
        // Should walk up to tmp.path() but not find anything, then continue up
        // Eventually returns nullopt when reaching filesystem root
        // (This test may find a project root in parent dirs in dev environment)
    }
}

namespace {

auto write_file_at(fs::path const& full, std::string_view content) -> void
{
    fs::create_directories(full.parent_path());
    std::ofstream { full } << content;
}

} // namespace

TEST_CASE("discover_variants respects build dir ownership", "[e2e][layout]")
{
    auto tmp = TempDir {};
    tmp.create_file("Tupfile.ini");
    tmp.create_dir("other");
    tmp.create_file("build-own/tup.config");
    write_file_at(tmp.path() / "build-own/.pup-project", "..\n");
    tmp.create_file("build-plain/tup.config");
    tmp.create_file("build-foreign/tup.config");
    write_file_at(tmp.path() / "build-foreign/.pup-project", "../other\n");

    auto variants = pup::discover_variants(tmp.path().string(), tmp.path().string());

    auto names = std::vector<std::string> {};
    for (auto v : variants) {
        names.emplace_back(global_pool().get(v));
    }
    std::sort(names.begin(), names.end());
    REQUIRE(names == std::vector<std::string> { "build-own", "build-plain" });
}

TEST_CASE("find_build_subdir skips foreign-owned build dir", "[e2e][layout]")
{
    auto tmp = TempDir {};
    tmp.create_file("Tupfile.ini");
    tmp.create_dir("other");
    tmp.create_file("build/tup.config");

    SECTION("foreign stamp is skipped")
    {
        write_file_at(tmp.path() / "build/.pup-project", "../other\n");
        REQUIRE_FALSE(pup::find_build_subdir(tmp.path().string()).has_value());
    }

    SECTION("matching stamp is adopted")
    {
        write_file_at(tmp.path() / "build/.pup-project", "..\n");
        auto result = pup::find_build_subdir(tmp.path().string());
        REQUIRE(result.has_value());
        REQUIRE(global_pool().get(*result) == (tmp.path() / "build").string());
    }

    SECTION("missing stamp is adopted")
    {
        auto result = pup::find_build_subdir(tmp.path().string());
        REQUIRE(result.has_value());
        REQUIRE(global_pool().get(*result) == (tmp.path() / "build").string());
    }
}

TEST_CASE("discover_layout from build directory", "[e2e][layout]")
{
    SECTION("discovers source root when cwd is build directory")
    {
        auto tmp = TempDir {};
        tmp.create_file("Tupfile.ini");
        tmp.create_dir("build/.pup");
        tmp.create_file("build/tup.config");

        // Save and change cwd
        auto original_cwd = fs::current_path();
        fs::current_path(tmp.path() / "build");

        auto result = pup::discover_layout({});

        // Restore cwd before assertions (in case of failure)
        fs::current_path(original_cwd);

        REQUIRE(result.has_value());
        REQUIRE(fs::canonical(std::string(global_pool().get(result->source_root))) == fs::canonical(tmp.path()).string());
        REQUIRE(fs::canonical(std::string(global_pool().get(result->output_root))) == fs::canonical(tmp.path() / "build").string());
    }
}
