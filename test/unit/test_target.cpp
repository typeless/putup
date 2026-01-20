// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/cli/target.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

/// RAII helper to create a temporary directory tree for testing
class TempDir {
public:
    TempDir()
        : path_(fs::temp_directory_path() / ("pup_target_test_" + std::to_string(std::rand())))
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

    auto create_file(fs::path const& rel, std::string const& content = "") -> void
    {
        auto full = path_ / rel;
        fs::create_directories(full.parent_path());
        auto ofs = std::ofstream { full };
        if (!content.empty())
            ofs << content;
    }

    auto create_dir(fs::path const& rel) -> void { fs::create_directories(path_ / rel); }

private:
    fs::path path_;
};

} // namespace

SCENARIO("Target parsing - variant detection", "[target]")
{
    GIVEN("a project with build-debug/tup.config")
    {
        auto tmp = TempDir {};
        tmp.create_file("Tupfile.ini");
        tmp.create_file("build-debug/tup.config");
        tmp.create_dir("src/lib");

        WHEN("parsing 'build-debug'")
        {
            auto result = pup::parse_target(tmp.path(), "build-debug");

            THEN("variant=build-debug, scope=empty, is_output=false")
            {
                REQUIRE(result.has_value());
                REQUIRE(result->variant.has_value());
                REQUIRE(*result->variant == "build-debug");
                REQUIRE(result->scope_or_output.empty());
                REQUIRE_FALSE(result->is_output);
            }
        }

        WHEN("parsing 'build-debug/src/lib'")
        {
            tmp.create_dir("build-debug/src/lib");
            auto result = pup::parse_target(tmp.path(), "build-debug/src/lib");

            THEN("variant=build-debug, scope=src/lib, is_output=false")
            {
                REQUIRE(result.has_value());
                REQUIRE(result->variant.has_value());
                REQUIRE(*result->variant == "build-debug");
                REQUIRE(result->scope_or_output == "src/lib");
                REQUIRE_FALSE(result->is_output);
            }
        }

        WHEN("parsing 'build-debug/src/lib/foo.o'")
        {
            tmp.create_file("build-debug/src/lib/foo.o");
            auto result = pup::parse_target(tmp.path(), "build-debug/src/lib/foo.o");

            THEN("variant=build-debug, scope=src/lib/foo.o, is_output=true")
            {
                REQUIRE(result.has_value());
                REQUIRE(result->variant.has_value());
                REQUIRE(*result->variant == "build-debug");
                REQUIRE(result->scope_or_output == "src/lib/foo.o");
                REQUIRE(result->is_output);
            }
        }

        WHEN("parsing 'src/lib'")
        {
            auto result = pup::parse_target(tmp.path(), "src/lib");

            THEN("variant=nullopt, scope=src/lib, is_output=false")
            {
                REQUIRE(result.has_value());
                REQUIRE_FALSE(result->variant.has_value());
                REQUIRE(result->scope_or_output == "src/lib");
                REQUIRE_FALSE(result->is_output);
            }
        }

        WHEN("parsing 'src/lib/foo.o'")
        {
            tmp.create_file("src/lib/foo.o");
            auto result = pup::parse_target(tmp.path(), "src/lib/foo.o");

            THEN("variant=nullopt, scope=src/lib/foo.o, is_output=true")
            {
                REQUIRE(result.has_value());
                REQUIRE_FALSE(result->variant.has_value());
                REQUIRE(result->scope_or_output == "src/lib/foo.o");
                REQUIRE(result->is_output);
            }
        }
    }
}

SCENARIO("Target parsing - glob expansion", "[target]")
{
    GIVEN("variants build-debug and build-release")
    {
        auto tmp = TempDir {};
        tmp.create_file("Tupfile.ini");
        tmp.create_file("build-debug/tup.config");
        tmp.create_file("build-release/tup.config");
        tmp.create_dir("src/lib");

        WHEN("parsing 'build-*'")
        {
            auto result = pup::expand_glob_target(tmp.path(), "build-*");

            THEN("expands to [build-debug, build-release]")
            {
                REQUIRE(result.size() == 2);
                auto variants = std::set<std::string> {};
                for (auto const& t : result) {
                    REQUIRE(t.variant.has_value());
                    variants.insert(t.variant->string());
                }
                REQUIRE(variants.count("build-debug") == 1);
                REQUIRE(variants.count("build-release") == 1);
            }
        }

        WHEN("parsing 'build-*/src/lib'")
        {
            tmp.create_dir("build-debug/src/lib");
            tmp.create_dir("build-release/src/lib");
            auto result = pup::expand_glob_target(tmp.path(), "build-*/src/lib");

            THEN("expands to [(build-debug, src/lib), (build-release, src/lib)]")
            {
                REQUIRE(result.size() == 2);
                for (auto const& t : result) {
                    REQUIRE(t.variant.has_value());
                    REQUIRE(t.scope_or_output == "src/lib");
                    REQUIRE_FALSE(t.is_output);
                }
            }
        }

        WHEN("parsing 'build-*/src/lib/foo.o'")
        {
            tmp.create_file("build-debug/src/lib/foo.o");
            tmp.create_file("build-release/src/lib/foo.o");
            auto result = pup::expand_glob_target(tmp.path(), "build-*/src/lib/foo.o");

            THEN("expands to [(build-debug, src/lib/foo.o), (build-release, src/lib/foo.o)]")
            {
                REQUIRE(result.size() == 2);
                for (auto const& t : result) {
                    REQUIRE(t.variant.has_value());
                    REQUIRE(t.scope_or_output == "src/lib/foo.o");
                    REQUIRE(t.is_output);
                }
            }
        }
    }
}

SCENARIO("Target parsing - error cases", "[target]")
{
    GIVEN("a project with source file src/lib/foo.c")
    {
        auto tmp = TempDir {};
        tmp.create_file("Tupfile.ini");
        tmp.create_file("src/lib/foo.c", "int main() {}");

        WHEN("parsing 'src/lib/foo.c' as output target")
        {
            auto result = pup::parse_target(tmp.path(), "src/lib/foo.c");

            THEN("returns error: source file not build output")
            {
                REQUIRE_FALSE(result.has_value());
                REQUIRE(result.error().message.find("source") != std::string::npos);
            }
        }
    }

    GIVEN("a nonexistent path with nonexistent parent")
    {
        auto tmp = TempDir {};
        tmp.create_file("Tupfile.ini");

        WHEN("parsing 'no_such_dir/nonexistent'")
        {
            auto result = pup::parse_target(tmp.path(), "no_such_dir/nonexistent");

            THEN("returns error: path not found")
            {
                REQUIRE_FALSE(result.has_value());
                REQUIRE(result.error().message.find("not found") != std::string::npos);
            }
        }
    }

    GIVEN("a nonexistent path with existing parent")
    {
        auto tmp = TempDir {};
        tmp.create_file("Tupfile.ini");

        WHEN("parsing 'nonexistent'")
        {
            auto result = pup::parse_target(tmp.path(), "nonexistent");

            THEN("treats as potential output target (validation deferred to build)")
            {
                REQUIRE(result.has_value());
                REQUIRE(result->is_output);
            }
        }
    }
}

SCENARIO("Target parsing - consistency rule", "[target]")
{
    GIVEN("variants build-debug and build-release")
    {
        auto tmp = TempDir {};
        tmp.create_file("Tupfile.ini");
        tmp.create_file("build-debug/tup.config");
        tmp.create_file("build-release/tup.config");
        tmp.create_dir("src");
        tmp.create_dir("test");

        WHEN("parsing ['build-debug/src', 'src/test']")
        {
            tmp.create_dir("build-debug/src");
            tmp.create_dir("src/test");
            auto targets = std::vector<std::string> { "build-debug/src", "src/test" };
            auto result = pup::validate_target_consistency(tmp.path(), targets);

            THEN("returns error: cannot mix variant-specific and all-variant targets")
            {
                REQUIRE_FALSE(result.has_value());
                REQUIRE(result.error().message.find("mix") != std::string::npos);
            }
        }

        WHEN("parsing ['build-debug', 'src']")
        {
            auto targets = std::vector<std::string> { "build-debug", "src" };
            auto result = pup::validate_target_consistency(tmp.path(), targets);

            THEN("returns error: cannot mix variant-specific and all-variant targets")
            {
                REQUIRE_FALSE(result.has_value());
                REQUIRE(result.error().message.find("mix") != std::string::npos);
            }
        }

        WHEN("parsing ['build-debug/src', 'build-release/test']")
        {
            tmp.create_dir("build-debug/src");
            tmp.create_dir("build-release/test");
            auto targets = std::vector<std::string> { "build-debug/src", "build-release/test" };
            auto result = pup::validate_target_consistency(tmp.path(), targets);

            THEN("succeeds: both have explicit variants")
            {
                REQUIRE(result.has_value());
            }
        }

        WHEN("parsing ['src', 'test']")
        {
            auto targets = std::vector<std::string> { "src", "test" };
            auto result = pup::validate_target_consistency(tmp.path(), targets);

            THEN("succeeds: neither has explicit variant")
            {
                REQUIRE(result.has_value());
            }
        }
    }
}
