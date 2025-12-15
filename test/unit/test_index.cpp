// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "catch_amalgamated.hpp"
#include "pup/index/entry.hpp"
#include "pup/index/format.hpp"
#include "pup/index/reader.hpp"
#include "pup/index/writer.hpp"

#include <cstring>
#include <filesystem>

using namespace pup;
using namespace pup::index;

TEST_CASE("Index format struct sizes", "[index]")
{
    SECTION("RawHeader is 64 bytes")
    {
        REQUIRE(sizeof(RawHeader) == 64);
    }

    SECTION("RawFileEntry is 96 bytes")
    {
        REQUIRE(sizeof(RawFileEntry) == 96);
    }

    SECTION("RawCommandEntry is 64 bytes")
    {
        REQUIRE(sizeof(RawCommandEntry) == 64);
    }

    SECTION("RawEdge is 24 bytes")
    {
        REQUIRE(sizeof(RawEdge) == 24);
    }

    SECTION("RawFooter is 32 bytes")
    {
        REQUIRE(sizeof(RawFooter) == 32);
    }
}

TEST_CASE("RawFileEntry helpers", "[index]")
{
    auto entry = RawFileEntry {};

    SECTION("file size helpers")
    {
        entry.size = 0x123456789ABCDEF0ULL;
        REQUIRE(get_file_size(entry) == 0x123456789ABCDEF0ULL);

        entry.size = 0;
        REQUIRE(get_file_size(entry) == 0);

        entry.size = 0xFFFFFFFFFFFFFFFFULL;
        REQUIRE(get_file_size(entry) == 0xFFFFFFFFFFFFFFFFULL);
    }

    SECTION("node flags helpers")
    {
        set_node_flags(entry, NodeFlags::Modified | NodeFlags::Created);
        REQUIRE(get_node_flags(entry) == (NodeFlags::Modified | NodeFlags::Created));

        set_node_flags(entry, NodeFlags::None);
        REQUIRE(get_node_flags(entry) == NodeFlags::None);
    }
}

TEST_CASE("FileEntry conversion", "[index]")
{
    auto file = FileEntry {
        .id = 42,
        .parent_id = 1,
        .src_id = 0,
        .type = NodeType::File,
        .flags = NodeFlags::Modified,
        .name = "main.cpp",
        .path = {}, // computed from parent chain, not serialized
        .size = 1024,
        .content_hash = {},
    };

    // Set some hash bytes
    file.content_hash[0] = std::byte { 0xAB };
    file.content_hash[31] = std::byte { 0xCD };

    auto raw = file.to_raw(200);

    REQUIRE(raw.id == 42);
    REQUIRE(raw.parent_id == 1);
    REQUIRE(raw.type == static_cast<std::uint8_t>(NodeType::File));
    REQUIRE(get_file_size(raw) == 1024);
    REQUIRE(raw.name_offset == 200);
    REQUIRE(raw.name_length == 8);
    REQUIRE(raw.content_hash[0] == std::byte { 0xAB });
    REQUIRE(raw.content_hash[31] == std::byte { 0xCD });

    auto restored = FileEntry::from_raw(raw, "main.cpp");

    REQUIRE(restored.id == file.id);
    REQUIRE(restored.parent_id == file.parent_id);
    REQUIRE(restored.type == file.type);
    REQUIRE(restored.flags == file.flags);
    REQUIRE(restored.name == file.name);
    REQUIRE(restored.size == file.size);
    REQUIRE(restored.content_hash == file.content_hash);
}

TEST_CASE("CommandEntry conversion", "[index]")
{
    auto cmd = CommandEntry {
        .id = 100,
        .dir_id = 5,
        .command = "gcc -c main.c -o main.o",
        .display = "CC main.c",
        .env = "CC=gcc",
        .flags = 1,
    };

    auto raw = cmd.to_raw(0, 50, 100);

    REQUIRE(raw.id == 100);
    REQUIRE(raw.dir_id == 5);
    REQUIRE(raw.cmd_offset == 0);
    REQUIRE(raw.cmd_length == 23);
    REQUIRE(raw.display_offset == 50);
    REQUIRE(raw.display_length == 9);
    REQUIRE(raw.env_offset == 100);
    REQUIRE(raw.env_length == 6);
    REQUIRE(raw.flags == 1);

    auto restored = CommandEntry::from_raw(raw, cmd.command, cmd.display, cmd.env);

    REQUIRE(restored.id == cmd.id);
    REQUIRE(restored.dir_id == cmd.dir_id);
    REQUIRE(restored.command == cmd.command);
    REQUIRE(restored.display == cmd.display);
    REQUIRE(restored.env == cmd.env);
    REQUIRE(restored.flags == cmd.flags);
}

TEST_CASE("EdgeEntry conversion", "[index]")
{
    SECTION("Sticky edge")
    {
        auto edge = EdgeEntry {
            .from = 10,
            .to = 20,
            .type = LinkType::Sticky,
            .group_cmd_id = 5,
        };

        auto raw = edge.to_raw();

        REQUIRE(raw.from_id == 10);
        REQUIRE(raw.to_id == 20);
        REQUIRE(raw.type == static_cast<std::uint8_t>(LinkType::Sticky));
        REQUIRE(raw.group_cmd_id == 5);

        auto restored = EdgeEntry::from_raw(raw);

        REQUIRE(restored.from == edge.from);
        REQUIRE(restored.to == edge.to);
        REQUIRE(restored.type == edge.type);
        REQUIRE(restored.group_cmd_id == edge.group_cmd_id);
    }

    SECTION("Implicit edge (header dependency)")
    {
        auto edge = EdgeEntry {
            .from = 100,
            .to = 200,
            .type = LinkType::Implicit,
            .group_cmd_id = 0,
        };

        auto raw = edge.to_raw();

        REQUIRE(raw.from_id == 100);
        REQUIRE(raw.to_id == 200);
        REQUIRE(raw.type == static_cast<std::uint8_t>(LinkType::Implicit));
        REQUIRE(raw.group_cmd_id == 0);

        auto restored = EdgeEntry::from_raw(raw);

        REQUIRE(restored.from == edge.from);
        REQUIRE(restored.to == edge.to);
        REQUIRE(restored.type == LinkType::Implicit);
        REQUIRE(restored.group_cmd_id == edge.group_cmd_id);
    }
}

TEST_CASE("Index in-memory operations", "[index]")
{
    auto index = Index {};

    REQUIRE(index.empty());
    REQUIRE(index.file_count() == 0);
    REQUIRE(index.command_count() == 0);
    REQUIRE(index.edge_count() == 0);

    SECTION("add and find files")
    {
        index.add_file(FileEntry { .id = 1, .name = "foo.c", .path = "foo.c" });
        index.add_file(FileEntry { .id = 2, .name = "bar.c", .path = "bar.c" });

        REQUIRE(index.file_count() == 2);
        REQUIRE_FALSE(index.empty());

        auto* found = index.find_file("foo.c");
        REQUIRE(found != nullptr);
        REQUIRE(found->id == 1);

        REQUIRE(index.find_file("nonexistent") == nullptr);

        auto* by_id = index.find_file_by_id(2);
        REQUIRE(by_id != nullptr);
        REQUIRE(by_id->name == "bar.c");
    }

    SECTION("add and find commands")
    {
        index.add_command(CommandEntry { .id = 10, .command = "gcc foo.c" });
        index.add_command(CommandEntry { .id = 11, .command = "gcc bar.c" });

        REQUIRE(index.command_count() == 2);

        auto* found = index.find_command_by_id(10);
        REQUIRE(found != nullptr);
        REQUIRE(found->command == "gcc foo.c");

        REQUIRE(index.find_command_by_id(999) == nullptr);
    }

    SECTION("add and query edges")
    {
        index.add_edge(EdgeEntry { .from = 1, .to = 10 });
        index.add_edge(EdgeEntry { .from = 10, .to = 2 });
        index.add_edge(EdgeEntry { .from = 1, .to = 11 });

        REQUIRE(index.edge_count() == 3);

        auto from_1 = index.edges_from(1);
        REQUIRE(from_1.size() == 2);

        auto to_2 = index.edges_to(2);
        REQUIRE(to_2.size() == 1);
        REQUIRE(to_2[0]->from == 10);
    }

    SECTION("clear")
    {
        index.add_file(FileEntry { .id = 1, .name = "test.c" });
        index.add_command(CommandEntry { .id = 10 });
        index.add_edge(EdgeEntry { .from = 1, .to = 10 });

        REQUIRE_FALSE(index.empty());

        index.clear();

        REQUIRE(index.empty());
        REQUIRE(index.file_count() == 0);
        REQUIRE(index.command_count() == 0);
        REQUIRE(index.edge_count() == 0);
    }
}

TEST_CASE("Index serialization roundtrip", "[index]")
{
    auto index = Index {};

    // Add directories first (for parent chain)
    index.add_file(FileEntry {
        .id = 100,
        .parent_id = 0,
        .type = NodeType::Directory,
        .name = "src",
    });

    index.add_file(FileEntry {
        .id = 101,
        .parent_id = 0,
        .type = NodeType::Directory,
        .name = "build",
    });

    // Add some files
    index.add_file(FileEntry {
        .id = 1,
        .parent_id = 100, // src
        .type = NodeType::File,
        .name = "main.cpp",
        .size = 1024,
    });

    index.add_file(FileEntry {
        .id = 2,
        .parent_id = 101, // build
        .type = NodeType::Generated,
        .name = "main.o",
        .size = 4096,
    });

    // Add a command
    index.add_command(CommandEntry {
        .id = 10,
        .dir_id = 0,
        .command = "g++ -c src/main.cpp -o build/main.o",
        .display = "CXX main.cpp",
    });

    // Add a header file (implicit dependency, root-level with path-like name)
    index.add_file(FileEntry {
        .id = 3,
        .parent_id = 0,
        .type = NodeType::File,
        .name = "/usr/include/stdio.h",
        .size = 8192,
    });

    // Add edges (including implicit header dependency)
    index.add_edge(EdgeEntry { .from = 1, .to = 10, .type = LinkType::Normal });
    index.add_edge(EdgeEntry { .from = 10, .to = 2, .type = LinkType::Normal });
    index.add_edge(EdgeEntry { .from = 3, .to = 10, .type = LinkType::Implicit });

    // Serialize
    auto writer = IndexWriter {};
    auto data = writer.serialize(index);
    REQUIRE(data.has_value());
    REQUIRE(data->size() > sizeof(RawHeader) + sizeof(RawFooter));

    // Write to temp file and read back
    auto temp_path = std::filesystem::temp_directory_path() / "pup_test_index";

    auto write_result = writer.write(temp_path, index);
    REQUIRE(write_result.has_value());

    // Verify file exists
    REQUIRE(std::filesystem::exists(temp_path));

    // Read back
    auto reader_result = IndexReader::open(temp_path);
    REQUIRE(reader_result.has_value());

    auto& reader = *reader_result;
    REQUIRE(reader.is_open());

    // Check header
    auto const* hdr = reader.header();
    REQUIRE(hdr != nullptr);
    REQUIRE(std::memcmp(hdr->magic.data(), INDEX_MAGIC.data(), 4) == 0);
    REQUIRE(hdr->version == INDEX_VERSION);
    REQUIRE(hdr->file_count == 5); // 2 dirs + 3 files
    REQUIRE(hdr->command_count == 1);
    REQUIRE(hdr->edge_count == 3);

    // Verify checksum
    REQUIRE(reader.verify_checksum());

    // Read full index
    auto read_result = reader.read();
    REQUIRE(read_result.has_value());

    auto& restored = *read_result;
    REQUIRE(restored.file_count() == 5);
    REQUIRE(restored.command_count() == 1);
    REQUIRE(restored.edge_count() == 3);

    // Verify file content (paths are computed from parent chain)
    auto* file1 = restored.find_file("src/main.cpp");
    REQUIRE(file1 != nullptr);
    REQUIRE(file1->id == 1);
    REQUIRE(file1->size == 1024);

    auto* file2 = restored.find_file("build/main.o");
    REQUIRE(file2 != nullptr);
    REQUIRE(file2->id == 2);
    REQUIRE(file2->type == NodeType::Generated);

    // Verify command
    auto* cmd = restored.find_command_by_id(10);
    REQUIRE(cmd != nullptr);
    REQUIRE(cmd->command == "g++ -c src/main.cpp -o build/main.o");
    REQUIRE(cmd->display == "CXX main.cpp");

    // Verify header file (implicit dependency)
    auto* header = restored.find_file("/usr/include/stdio.h");
    REQUIRE(header != nullptr);
    REQUIRE(header->id == 3);
    REQUIRE(header->size == 8192);

    // Verify implicit edge
    auto edges_to_cmd = restored.edges_to(10);
    REQUIRE(edges_to_cmd.size() == 2);
    auto found_implicit = false;
    for (auto const* edge : edges_to_cmd) {
        if (edge->type == LinkType::Implicit) {
            REQUIRE(edge->from == 3);
            found_implicit = true;
        }
    }
    REQUIRE(found_implicit);

    // Cleanup
    std::filesystem::remove(temp_path);
}

TEST_CASE("Index reader validation", "[index]")
{
    SECTION("non-existent file")
    {
        auto result = IndexReader::open("/nonexistent/path/to/index");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("is_valid_index")
    {
        REQUIRE_FALSE(IndexReader::is_valid_index("/nonexistent"));

        // Create a valid index
        auto index = Index {};
        index.add_file(FileEntry { .id = 1, .name = "test.c" });

        auto temp_path = std::filesystem::temp_directory_path() / "pup_valid_test";
        auto writer = IndexWriter {};
        (void)writer.write(temp_path, index);

        REQUIRE(IndexReader::is_valid_index(temp_path));

        std::filesystem::remove(temp_path);
    }
}
