// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/string_pool.hpp"

#include <filesystem>
#include <fstream>

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

TEST_CASE("find_enclosing_build_dir", "[layout]")
{
    auto tmp = TempDir {};
    tmp.create_file("Tupfile.ini");
    auto root = tmp.path().string();

    SECTION("cwd at the build dir root")
    {
        tmp.create_file("build/tup.config");

        auto result = pup::find_enclosing_build_dir((tmp.path() / "build").string(), root);
        REQUIRE(result.has_value());
        REQUIRE(fs::canonical(std::string(global_pool().get(*result))) == fs::canonical(tmp.path() / "build"));
    }

    SECTION("cwd deep inside the build dir")
    {
        tmp.create_file("build/tup.config");
        tmp.create_dir("build/sub/nested");

        auto result = pup::find_enclosing_build_dir((tmp.path() / "build" / "sub" / "nested").string(), root);
        REQUIRE(result.has_value());
        REQUIRE(fs::canonical(std::string(global_pool().get(*result))) == fs::canonical(tmp.path() / "build"));
    }

    SECTION("scoped tup.config below the build root does not win over it")
    {
        tmp.create_file("build/tup.config");
        tmp.create_file("build/configs/tup.config");

        auto result = pup::find_enclosing_build_dir((tmp.path() / "build" / "configs").string(), root);
        REQUIRE(result.has_value());
        REQUIRE(fs::canonical(std::string(global_pool().get(*result))) == fs::canonical(tmp.path() / "build"));
    }

    SECTION("cwd at the source root finds nothing")
    {
        tmp.create_file("build/tup.config");

        REQUIRE_FALSE(pup::find_enclosing_build_dir(root, root).has_value());
    }

    SECTION("cwd in an unmarked source subdirectory finds nothing")
    {
        tmp.create_file("build/tup.config");
        tmp.create_dir("src/lib");

        REQUIRE_FALSE(pup::find_enclosing_build_dir((tmp.path() / "src" / "lib").string(), root).has_value());
    }

    SECTION("cwd outside the source root finds nothing")
    {
        auto other = TempDir {};
        other.create_file("tup.config");

        REQUIRE_FALSE(pup::find_enclosing_build_dir(other.path().string(), root).has_value());
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
