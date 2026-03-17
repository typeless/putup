// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "e2e_fixture.hpp"
#include "pup/platform/file_io.hpp"

#include <cstring>
#include <fstream>

using namespace pup::platform;
using namespace pup::test;

SCENARIO("MappedFile provides read-only access to file contents", "[e2e][platform][file_io]")
{
    GIVEN("a file with known contents")
    {
        auto f = E2EFixture { "simple_c" };
        auto test_content = std::string { "Hello, World!" };
        f.write_file("test.bin", test_content);

        WHEN("the file is memory-mapped")
        {
            auto result = MappedFile::open((f.workdir() / "test.bin").string());

            THEN("the operation succeeds")
            {
                REQUIRE(result.has_value());
            }

            THEN("the mapped file is open")
            {
                REQUIRE(result->is_open());
            }

            THEN("the size matches the file content")
            {
                REQUIRE(result->size() == test_content.size());
            }

            THEN("the contents can be read correctly")
            {
                REQUIRE(std::memcmp(result->data(), test_content.data(), test_content.size()) == 0);
            }
        }
    }
}

SCENARIO("MappedFile handles missing files", "[platform][file_io]")
{
    GIVEN("a path to a non-existent file")
    {
        auto path = std::string { "/tmp/pup_test_nonexistent_12345.bin" };

        WHEN("attempting to memory-map the file")
        {
            auto result = MappedFile::open(path);

            THEN("the operation fails with an error")
            {
                REQUIRE_FALSE(result.has_value());
                REQUIRE(result.error().code == pup::ErrorCode::IoError);
            }
        }
    }
}

SCENARIO("MappedFile is move-only", "[e2e][platform][file_io]")
{
    GIVEN("a memory-mapped file")
    {
        auto f = E2EFixture { "simple_c" };
        f.write_file("test.bin", "test data");

        auto original = MappedFile::open((f.workdir() / "test.bin").string());
        REQUIRE(original.has_value());
        auto original_data = original->data();
        auto original_size = original->size();

        WHEN("moved to another MappedFile")
        {
            auto moved = MappedFile { std::move(*original) };

            THEN("the moved-from object is no longer open")
            {
                REQUIRE_FALSE(original->is_open());
            }

            THEN("the moved-to object has the same data")
            {
                REQUIRE(moved.is_open());
                REQUIRE(moved.data() == original_data);
                REQUIRE(moved.size() == original_size);
            }
        }
    }
}

TEST_CASE("stat_file returns file metadata", "[e2e][platform][file_io]")
{
    auto f = E2EFixture { "simple_c" };
    auto content = std::string { "test content with known size" };
    f.write_file("stat_test.txt", content);

    SECTION("returns correct size for existing file")
    {
        auto result = stat_file((f.workdir() / "stat_test.txt").string());
        REQUIRE(result.has_value());
        REQUIRE(result->size == content.size());
    }

    SECTION("returns error for non-existent file")
    {
        auto result = stat_file((f.workdir() / "nonexistent.txt").string());
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == pup::ErrorCode::IoError);
    }
}

TEST_CASE("atomic_write creates file atomically", "[e2e][platform][file_io]")
{
    auto f = E2EFixture { "simple_c" };
    auto content = std::string { "atomic write test content" };
    auto content_bytes = std::span<std::byte const> {
        reinterpret_cast<std::byte const*>(content.data()),
        content.size()
    };

    SECTION("creates new file with correct contents")
    {
        auto path = (f.workdir() / "atomic_test.txt").string();

        auto result = atomic_write(path, content_bytes);
        REQUIRE(result.has_value());

        auto written = f.read_file("atomic_test.txt");
        REQUIRE(written == content);
    }

    SECTION("overwrites existing file")
    {
        f.write_file("existing.txt", "old content");
        auto path = (f.workdir() / "existing.txt").string();

        auto result = atomic_write(path, content_bytes);
        REQUIRE(result.has_value());

        auto written = f.read_file("existing.txt");
        REQUIRE(written == content);
    }

    SECTION("creates parent directories if needed")
    {
        auto path = (f.workdir() / "subdir" / "nested" / "atomic_test.txt").string();

        auto result = atomic_write(path, content_bytes);
        REQUIRE(result.has_value());

        REQUIRE(std::filesystem::exists(path));
    }
}
