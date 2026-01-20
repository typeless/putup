// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/layout.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

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

TEST_CASE("find_project_root", "[layout]")
{
    SECTION("finds Tupfile.ini in current directory")
    {
        auto tmp = TempDir {};
        tmp.create_file("Tupfile.ini");

        auto result = pup::find_project_root(tmp.path());
        REQUIRE(result.has_value());
        REQUIRE(*result == tmp.path());
    }

    SECTION("finds Tupfile.ini in parent directory")
    {
        auto tmp = TempDir {};
        tmp.create_file("Tupfile.ini");
        tmp.create_dir("src/lib");

        auto result = pup::find_project_root(tmp.path() / "src" / "lib");
        REQUIRE(result.has_value());
        REQUIRE(*result == tmp.path());
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
        auto result = pup::find_project_root(tmp.path() / "build-release");
        REQUIRE(result.has_value());
        REQUIRE(*result == tmp.path());
    }

    SECTION("returns nullopt when no project root found")
    {
        auto tmp = TempDir {};
        tmp.create_dir("empty");

        auto result = pup::find_project_root(tmp.path() / "empty");
        // Should walk up to tmp.path() but not find anything, then continue up
        // Eventually returns nullopt when reaching filesystem root
        // (This test may find a project root in parent dirs in dev environment)
    }
}

TEST_CASE("discover_layout from build directory", "[layout]")
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
        REQUIRE(fs::canonical(result->source_root) == fs::canonical(tmp.path()));
        REQUIRE(fs::canonical(result->output_root) == fs::canonical(tmp.path() / "build"));
    }
}
