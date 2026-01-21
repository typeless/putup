// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/index/entry.hpp"
#include "pup/index/format.hpp"
#include "pup/index/reader.hpp"
#include "pup/index/writer.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

using namespace pup;
using namespace pup::index;

namespace {

auto find_file_by_path(Index const& index, std::string_view path) -> FileEntry const*
{
    for (auto const& file : index.files()) {
        if (file.path == path)
            return &file;
    }
    return nullptr;
}

} // namespace

TEST_CASE("Index format struct sizes", "[index]")
{
    SECTION("RawHeader is 40 bytes")
    {
        REQUIRE(sizeof(RawHeader) == 40);
    }

    SECTION("RawFileEntry is 56 bytes")
    {
        REQUIRE(sizeof(RawFileEntry) == 56);
    }

    SECTION("RawCommandEntry is 16 bytes")
    {
        REQUIRE(sizeof(RawCommandEntry) == 16);
    }

    SECTION("RawEdge is 16 bytes")
    {
        REQUIRE(sizeof(RawEdge) == 16);
    }

    SECTION("RawFooter is 32 bytes")
    {
        REQUIRE(sizeof(RawFooter) == 32);
    }
}

TEST_CASE("RawFileEntry helpers", "[index]")
{
    auto entry = RawFileEntry {};

    SECTION("file size field")
    {
        entry.size = 0x123456789ABCDEF0ULL;
        REQUIRE(entry.size == 0x123456789ABCDEF0ULL);

        entry.size = 0;
        REQUIRE(entry.size == 0);

        entry.size = 0xFFFFFFFFFFFFFFFFULL;
        REQUIRE(entry.size == 0xFFFFFFFFFFFFFFFFULL);
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
        .path = {},
        .size = 1024,
        .content_hash = {},
    };

    file.content_hash[0] = std::byte { 0xAB };
    file.content_hash[31] = std::byte { 0xCD };

    auto raw = file.to_raw(200);

    REQUIRE(raw.parent_id == 1);
    REQUIRE(raw.type == static_cast<std::uint8_t>(NodeType::File));
    REQUIRE(raw.size == 1024);
    REQUIRE(raw.name_offset == 200);
    REQUIRE(raw.content_hash[0] == std::byte { 0xAB });
    REQUIRE(raw.content_hash[31] == std::byte { 0xCD });

    // ID is computed from array index (41 + 1 = 42)
    auto restored = FileEntry::from_raw(raw, "main.cpp", 41);

    REQUIRE(restored.id == 42);
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
        .id = make_command_id(5),
        .dir_id = 5,
        .command = "gcc -c main.c -o main.o",
        .display = "CC main.c",
        .env = "CC=gcc",
    };

    auto raw = cmd.to_raw(0, 50, 100);

    REQUIRE(raw.dir_id == 5);
    REQUIRE(raw.cmd_offset == 0);
    REQUIRE(raw.display_offset == 50);
    REQUIRE(raw.env_offset == 100);

    // ID is computed from array index (4 + 1 = 5, then make_command_id)
    auto restored = CommandEntry::from_raw(raw, cmd.command, cmd.display, cmd.env, 4);

    REQUIRE(restored.id == make_command_id(5));
    REQUIRE(restored.dir_id == cmd.dir_id);
    REQUIRE(restored.command == cmd.command);
    REQUIRE(restored.display == cmd.display);
    REQUIRE(restored.env == cmd.env);
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

        auto* by_id_1 = index.find_file_by_id(1);
        REQUIRE(by_id_1 != nullptr);
        REQUIRE(by_id_1->name == "foo.c");

        auto* by_id_2 = index.find_file_by_id(2);
        REQUIRE(by_id_2 != nullptr);
        REQUIRE(by_id_2->name == "bar.c");

        REQUIRE(index.find_file_by_id(999) == nullptr);
    }

    SECTION("add and find commands")
    {
        index.add_command(CommandEntry { .id = make_command_id(1), .command = "gcc foo.c" });
        index.add_command(CommandEntry { .id = make_command_id(2), .command = "gcc bar.c" });

        REQUIRE(index.command_count() == 2);

        auto* found = index.find_command_by_id(make_command_id(1));
        REQUIRE(found != nullptr);
        REQUIRE(found->command == "gcc foo.c");

        REQUIRE(index.find_command_by_id(make_command_id(999)) == nullptr);
    }

    SECTION("add and query edges")
    {
        auto cmd1 = make_command_id(1);
        auto cmd2 = make_command_id(2);
        index.add_edge(EdgeEntry { .from = 1, .to = cmd1 });
        index.add_edge(EdgeEntry { .from = cmd1, .to = 2 });
        index.add_edge(EdgeEntry { .from = 1, .to = cmd2 });

        REQUIRE(index.edge_count() == 3);

        auto from_1 = index.edges_from(1);
        REQUIRE(from_1.size() == 2);

        auto to_2 = index.edges_to(2);
        REQUIRE(to_2.size() == 1);
        REQUIRE(to_2[0]->from == cmd1);
    }

    SECTION("clear")
    {
        auto cmd1 = make_command_id(1);
        index.add_file(FileEntry { .id = 1, .name = "test.c" });
        index.add_command(CommandEntry { .id = cmd1 });
        index.add_edge(EdgeEntry { .from = 1, .to = cmd1 });

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
    // IDs must be consecutive and match array position (id = array_index + 1)
    // Files: 1, 2, 3, 4, 5 in insertion order
    // Commands: make_command_id(1)
    auto const cmd_id = make_command_id(1);

    auto index = Index {};

    // Add directories first (for parent chain)
    // File 1: src directory
    index.add_file(FileEntry {
        .id = 1,
        .parent_id = 0,
        .type = NodeType::Directory,
        .name = "src",
    });

    // File 2: build directory
    index.add_file(FileEntry {
        .id = 2,
        .parent_id = 0,
        .type = NodeType::Directory,
        .name = "build",
    });

    // File 3: main.cpp (parent is src, id=1)
    index.add_file(FileEntry {
        .id = 3,
        .parent_id = 1,
        .type = NodeType::File,
        .name = "main.cpp",
        .size = 1024,
    });

    // File 4: main.o (parent is build, id=2)
    index.add_file(FileEntry {
        .id = 4,
        .parent_id = 2,
        .type = NodeType::Generated,
        .name = "main.o",
        .size = 4096,
    });

    // File 5: header file (implicit dependency)
    index.add_file(FileEntry {
        .id = 5,
        .parent_id = 0,
        .type = NodeType::File,
        .name = "/usr/include/stdio.h",
        .size = 8192,
    });

    // Command 1
    index.add_command(CommandEntry {
        .id = cmd_id,
        .dir_id = 0,
        .command = "g++ -c src/main.cpp -o build/main.o",
        .display = "CXX main.cpp",
    });

    // Add edges (file 3 -> cmd, cmd -> file 4, file 5 -> cmd implicit)
    index.add_edge(EdgeEntry { .from = 3, .to = cmd_id, .type = LinkType::Normal });
    index.add_edge(EdgeEntry { .from = cmd_id, .to = 4, .type = LinkType::Normal });
    index.add_edge(EdgeEntry { .from = 5, .to = cmd_id, .type = LinkType::Implicit });

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
    REQUIRE(hdr->file_count == 5);
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
    auto* file1 = find_file_by_path(restored, "src/main.cpp");
    REQUIRE(file1 != nullptr);
    REQUIRE(file1->id == 3);
    REQUIRE(file1->size == 1024);

    auto* file2 = find_file_by_path(restored, "build/main.o");
    REQUIRE(file2 != nullptr);
    REQUIRE(file2->id == 4);
    REQUIRE(file2->type == NodeType::Generated);

    // Verify command (ID computed from position: make_command_id(0 + 1))
    auto* cmd = restored.find_command_by_id(cmd_id);
    REQUIRE(cmd != nullptr);
    REQUIRE(cmd->command == "g++ -c src/main.cpp -o build/main.o");
    REQUIRE(cmd->display == "CXX main.cpp");

    // Verify header file (implicit dependency)
    auto* header = find_file_by_path(restored, "/usr/include/stdio.h");
    REQUIRE(header != nullptr);
    REQUIRE(header->id == 5);
    REQUIRE(header->size == 8192);

    // Verify implicit edge
    auto edges_to_cmd = restored.edges_to(cmd_id);
    REQUIRE(edges_to_cmd.size() == 2);
    auto found_implicit = false;
    for (auto const* edge : edges_to_cmd) {
        if (edge->type == LinkType::Implicit) {
            REQUIRE(edge->from == 5);
            found_implicit = true;
        }
    }
    REQUIRE(found_implicit);

    // Cleanup
    std::filesystem::remove(temp_path);
}

TEST_CASE("Index ID contiguity requirement", "[index]")
{
    // This test documents a design constraint: IDs must be contiguous when
    // stored in the index. The index format assigns IDs from array position
    // on load (id = array_index + 1), so if there are gaps in stored IDs,
    // parent_id references will be broken after round-trip.
    //
    // The build system ensures ID contiguity by storing ALL node types
    // (including Ghost, Variable, Group) rather than skipping them.
    // This test verifies the consequence of violating this constraint.

    auto index = Index {};

    // Create entries with consecutive IDs (no gaps)
    index.add_file(FileEntry {
        .id = 1,
        .parent_id = 0,
        .type = NodeType::Directory,
        .name = "src",
    });

    index.add_file(FileEntry {
        .id = 2,
        .parent_id = 0,
        .type = NodeType::Directory,
        .name = "build",
    });

    // ID 3: placeholder (like a Ghost node would be stored)
    index.add_file(FileEntry {
        .id = 3,
        .parent_id = 0,
        .type = NodeType::Ghost,
        .name = "placeholder",
    });

    // ID 4: subdirectory under src (parent=1)
    index.add_file(FileEntry {
        .id = 4,
        .parent_id = 1,
        .type = NodeType::Directory,
        .name = "lib",
    });

    // ID 5: file under lib (parent=4)
    index.add_file(FileEntry {
        .id = 5,
        .parent_id = 4,
        .type = NodeType::File,
        .name = "foo.c",
        .size = 100,
    });

    // Serialize and read back
    auto temp_path = std::filesystem::temp_directory_path() / "pup_test_contiguous";
    auto writer = IndexWriter {};
    auto write_result = writer.write(temp_path, index);
    REQUIRE(write_result.has_value());

    auto reader_result = IndexReader::open(temp_path);
    REQUIRE(reader_result.has_value());

    auto read_result = reader_result->read();
    REQUIRE(read_result.has_value());

    auto& restored = *read_result;

    // With consecutive IDs, path reconstruction works correctly
    auto* foo = find_file_by_path(restored, "src/lib/foo.c");
    REQUIRE(foo != nullptr);
    REQUIRE(foo->size == 100);

    auto* lib = find_file_by_path(restored, "src/lib");
    REQUIRE(lib != nullptr);
    REQUIRE(lib->type == NodeType::Directory);

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

TEST_CASE("Index reader malicious data handling", "[index]")
{
    // Create a minimal valid index to use as base
    auto const cmd_id = make_command_id(1);
    auto index = Index {};
    index.add_file(FileEntry { .id = 1, .name = "test.c" });
    index.add_command(CommandEntry { .id = cmd_id, .command = "gcc test.c" });
    index.add_edge(EdgeEntry { .from = 1, .to = cmd_id });

    auto writer = IndexWriter {};
    auto data = writer.serialize(index);
    REQUIRE(data.has_value());

    auto temp_path = std::filesystem::temp_directory_path() / "pup_malicious_test";

    SECTION("file_offset beyond file size")
    {
        auto corrupted = *data;
        auto* hdr = reinterpret_cast<RawHeader*>(corrupted.data());
        hdr->file_offset = corrupted.size() + 1000;

        std::ofstream out { temp_path, std::ios::binary };
        out.write(reinterpret_cast<char*>(corrupted.data()), static_cast<std::streamsize>(corrupted.size()));
        out.close();

        auto reader_result = IndexReader::open(temp_path);
        REQUIRE(reader_result.has_value());

        auto files = reader_result->raw_files();
        REQUIRE(files.empty());

        std::filesystem::remove(temp_path);
    }

    SECTION("file_count causes overflow past file end")
    {
        auto corrupted = *data;
        auto* hdr = reinterpret_cast<RawHeader*>(corrupted.data());
        hdr->file_count = 0xFFFFFFFF;

        std::ofstream out { temp_path, std::ios::binary };
        out.write(reinterpret_cast<char*>(corrupted.data()), static_cast<std::streamsize>(corrupted.size()));
        out.close();

        auto reader_result = IndexReader::open(temp_path);
        REQUIRE(reader_result.has_value());

        auto files = reader_result->raw_files();
        REQUIRE(files.empty());

        std::filesystem::remove(temp_path);
    }

    SECTION("command_offset beyond file size")
    {
        auto corrupted = *data;
        auto* hdr = reinterpret_cast<RawHeader*>(corrupted.data());
        hdr->command_offset = corrupted.size() + 1000;

        std::ofstream out { temp_path, std::ios::binary };
        out.write(reinterpret_cast<char*>(corrupted.data()), static_cast<std::streamsize>(corrupted.size()));
        out.close();

        auto reader_result = IndexReader::open(temp_path);
        REQUIRE(reader_result.has_value());

        auto commands = reader_result->raw_commands();
        REQUIRE(commands.empty());

        std::filesystem::remove(temp_path);
    }

    SECTION("command_count causes overflow past file end")
    {
        auto corrupted = *data;
        auto* hdr = reinterpret_cast<RawHeader*>(corrupted.data());
        hdr->command_count = 0xFFFFFFFF;

        std::ofstream out { temp_path, std::ios::binary };
        out.write(reinterpret_cast<char*>(corrupted.data()), static_cast<std::streamsize>(corrupted.size()));
        out.close();

        auto reader_result = IndexReader::open(temp_path);
        REQUIRE(reader_result.has_value());

        auto commands = reader_result->raw_commands();
        REQUIRE(commands.empty());

        std::filesystem::remove(temp_path);
    }

    SECTION("edge_offset beyond file size")
    {
        auto corrupted = *data;
        auto* hdr = reinterpret_cast<RawHeader*>(corrupted.data());
        hdr->edge_offset = corrupted.size() + 1000;

        std::ofstream out { temp_path, std::ios::binary };
        out.write(reinterpret_cast<char*>(corrupted.data()), static_cast<std::streamsize>(corrupted.size()));
        out.close();

        auto reader_result = IndexReader::open(temp_path);
        REQUIRE(reader_result.has_value());

        auto edges = reader_result->raw_edges();
        REQUIRE(edges.empty());

        std::filesystem::remove(temp_path);
    }

    SECTION("edge_count causes overflow past file end")
    {
        auto corrupted = *data;
        auto* hdr = reinterpret_cast<RawHeader*>(corrupted.data());
        hdr->edge_count = 0xFFFFFFFF;

        std::ofstream out { temp_path, std::ios::binary };
        out.write(reinterpret_cast<char*>(corrupted.data()), static_cast<std::streamsize>(corrupted.size()));
        out.close();

        auto reader_result = IndexReader::open(temp_path);
        REQUIRE(reader_result.has_value());

        auto edges = reader_result->raw_edges();
        REQUIRE(edges.empty());

        std::filesystem::remove(temp_path);
    }

    SECTION("offset at boundary but count overflows")
    {
        auto corrupted = *data;
        auto* hdr = reinterpret_cast<RawHeader*>(corrupted.data());
        // Set offset to valid position, but count would overflow
        hdr->file_offset = sizeof(RawHeader);
        hdr->file_count = (corrupted.size() / sizeof(RawFileEntry)) + 100;

        std::ofstream out { temp_path, std::ios::binary };
        out.write(reinterpret_cast<char*>(corrupted.data()), static_cast<std::streamsize>(corrupted.size()));
        out.close();

        auto reader_result = IndexReader::open(temp_path);
        REQUIRE(reader_result.has_value());

        auto files = reader_result->raw_files();
        REQUIRE(files.empty());

        std::filesystem::remove(temp_path);
    }

    SECTION("string offset beyond file size")
    {
        auto corrupted = *data;
        auto* hdr = reinterpret_cast<RawHeader*>(corrupted.data());
        hdr->string_offset = corrupted.size() + 1000;

        std::ofstream out { temp_path, std::ios::binary };
        out.write(reinterpret_cast<char*>(corrupted.data()), static_cast<std::streamsize>(corrupted.size()));
        out.close();

        auto reader_result = IndexReader::open(temp_path);
        REQUIRE(reader_result.has_value());

        // get_string should return empty for out-of-bounds
        auto str = reader_result->get_string(0);
        REQUIRE(str.empty());

        std::filesystem::remove(temp_path);
    }
}

TEST_CASE("StringTable overflow handling", "[index]")
{
    SECTION("string exceeding 64KB limit fails serialization")
    {
        auto index = Index {};

        // Create a string larger than 64KB (0xFFFF = 65535 bytes max)
        auto huge_name = std::string(65536, 'x');

        index.add_file(FileEntry { .id = 1, .parent_id = 0, .name = huge_name });

        auto writer = IndexWriter {};
        auto result = writer.serialize(index);

        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().message.find("64KB") != std::string::npos);
    }

    SECTION("string at 64KB limit succeeds")
    {
        auto index = Index {};

        // Create a string exactly at the 64KB limit (65535 bytes)
        auto max_name = std::string(65535, 'y');

        index.add_file(FileEntry { .id = 1, .parent_id = 0, .name = max_name });

        auto writer = IndexWriter {};
        auto result = writer.serialize(index);

        REQUIRE(result.has_value());
    }
}

TEST_CASE("StringTable deduplication", "[index]")
{
    SECTION("identical strings are deduplicated")
    {
        auto index = Index {};

        index.add_file(FileEntry { .id = 1, .parent_id = 0, .name = "main.cpp" });
        index.add_file(FileEntry { .id = 2, .parent_id = 0, .name = "main.cpp" });
        index.add_file(FileEntry { .id = 3, .parent_id = 0, .name = "main.cpp" });

        auto writer = IndexWriter {};
        auto data = writer.serialize(index);
        REQUIRE(data.has_value());

        // v6 length-prefixed: 2 (empty) + 2 (length) + 8 (main.cpp) = 12
        auto const* hdr = reinterpret_cast<RawHeader const*>(data->data());
        REQUIRE(hdr->string_table_size == 12);
    }

    SECTION("system header paths are deduplicated")
    {
        auto index = Index {};

        index.add_file(FileEntry { .id = 1, .parent_id = 0, .name = "/usr/include/stdio.h" });
        index.add_file(FileEntry { .id = 2, .parent_id = 0, .name = "/usr/include/stdio.h" });

        auto writer = IndexWriter {};
        auto data = writer.serialize(index);
        REQUIRE(data.has_value());

        // v6 length-prefixed: 2 (empty) + 2 (length) + 20 (path) = 24
        auto const* hdr = reinterpret_cast<RawHeader const*>(data->data());
        REQUIRE(hdr->string_table_size == 24);
    }

    SECTION("empty strings return offset 0")
    {
        auto index = Index {};

        index.add_command(CommandEntry { .id = make_command_id(1), .command = "gcc", .display = "", .env = "" });
        index.add_command(CommandEntry { .id = make_command_id(2), .command = "gcc", .display = "", .env = "" });

        auto writer = IndexWriter {};
        auto data = writer.serialize(index);
        REQUIRE(data.has_value());

        // v6 length-prefixed: 2 (empty) + 2 (length) + 3 (gcc) = 7
        auto const* hdr = reinterpret_cast<RawHeader const*>(data->data());
        REQUIRE(hdr->string_table_size == 7);
    }
}
