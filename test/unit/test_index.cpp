// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/cli/index_serialize.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/hash.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/graph/dag.hpp"
#include "pup/index/entry.hpp"
#include "pup/index/format.hpp"
#include "pup/index/reader.hpp"
#include "pup/index/writer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
auto sv(pup::StringId id) -> std::string_view { return pup::global_pool().get(id); }
} // namespace

using namespace pup;
using namespace pup::index;

namespace pup::graph {
// Graph-module-internal (defined in dag.cpp, not in public header)
auto add_condition_node(Graph& graph, ConditionNode node) -> Result<NodeId>;
} // namespace pup::graph

namespace {

auto intern(std::string_view s) -> StringId
{
    return global_pool().intern(s);
}

auto build_command_lookup(Index const& index)
    -> std::unordered_map<std::string, CommandEntry const*>
{
    auto lookup = std::unordered_map<std::string, CommandEntry const*> {};
    lookup.reserve(index.commands().size());

    for (auto const& cmd : index.commands()) {
        auto full_cmd = get_command_string(index, cmd);
        if (!pup::is_empty(full_cmd)) {
            lookup.emplace(std::string { sv(full_cmd) }, &cmd);
        }
    }

    return lookup;
}

auto find_file_by_path(Index const& index, std::string_view path) -> FileEntry const*
{
    for (auto const& file : index.files()) {
        if (global_pool().get(file.path) == path)
            return &file;
    }
    return nullptr;
}

} // namespace

TEST_CASE("Index format struct sizes", "[index]")
{
    SECTION("RawHeader is 56 bytes (v9)")
    {
        REQUIRE(sizeof(RawHeader) == 56);
    }

    SECTION("RawFileEntry is 64 bytes (v9)")
    {
        REQUIRE(sizeof(RawFileEntry) == 64);
    }

    SECTION("RawCommandEntry is 88 bytes (v20: + flags)")
    {
        REQUIRE(sizeof(RawCommandEntry) == 88);
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
        .name = intern("main.cpp"),
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
    // Distinct values: a roundtrip that swapped the two fields must fail.
    auto key = pup::Hash256 {};
    key[0] = std::byte { 0xAB };
    key[31] = std::byte { 0xCD };
    auto signature = pup::Hash256 {};
    signature[0] = std::byte { 0x12 };
    signature[31] = std::byte { 0x34 };

    auto cmd = CommandEntry {
        .id = node_id::make_command(5),
        .dir_id = 5,
        .instruction_pattern = intern("gcc -c %f -o %o"),
        .display = intern("CC main.c"),
        .env = intern("CC=gcc"),
        .key = key,
        .signature = signature,
        .must_rerun = true,
        .inputs = { 10 },
        .outputs = { 20 },
    };

    auto raw = cmd.to_raw(0, 50, 100);

    REQUIRE(raw.dir_id == 5);
    REQUIRE(raw.cmd_offset == 0);
    REQUIRE(raw.display_offset == 50);
    REQUIRE(raw.env_offset == 100);
    REQUIRE(raw.key == key);
    REQUIRE(raw.signature == signature);
    REQUIRE(raw.flags == pup::index::to_underlying(pup::index::CommandFlag::MustRerun));

    // ID is computed from array index (4 + 1 = 5, then node_id::make_command)
    auto& pool = global_pool();
    auto restored = CommandEntry::from_raw(
        raw, pool.get(cmd.instruction_pattern), pool.get(cmd.display), pool.get(cmd.env),
        pup::Vec<NodeId> { 10 }, pup::Vec<NodeId> { 20 }, 4
    );

    REQUIRE(restored.id == node_id::make_command(5));
    REQUIRE(restored.dir_id == cmd.dir_id);
    REQUIRE(restored.instruction_pattern == cmd.instruction_pattern);
    REQUIRE(restored.display == cmd.display);
    REQUIRE(restored.env == cmd.env);
    REQUIRE(restored.key == key);
    REQUIRE(restored.signature == signature);
    REQUIRE(restored.must_rerun);
    REQUIRE(restored.inputs == cmd.inputs);
    REQUIRE(restored.outputs == cmd.outputs);
}

TEST_CASE("EdgeEntry conversion", "[index]")
{
    SECTION("Sticky edge")
    {
        auto edge = EdgeEntry {
            .from = 10,
            .to = 20,
            .type = LinkType::Sticky,
        };

        auto raw = edge.to_raw();

        REQUIRE(raw.from_id == 10);
        REQUIRE(raw.to_id == 20);
        REQUIRE(raw.type == static_cast<std::uint8_t>(LinkType::Sticky));

        auto restored = EdgeEntry::from_raw(raw);

        REQUIRE(restored.from == edge.from);
        REQUIRE(restored.to == edge.to);
        REQUIRE(restored.type == edge.type);
    }

    SECTION("Implicit edge (header dependency)")
    {
        auto edge = EdgeEntry {
            .from = 100,
            .to = 200,
            .type = LinkType::Implicit,
        };

        auto raw = edge.to_raw();

        REQUIRE(raw.from_id == 100);
        REQUIRE(raw.to_id == 200);
        REQUIRE(raw.type == static_cast<std::uint8_t>(LinkType::Implicit));

        auto restored = EdgeEntry::from_raw(raw);

        REQUIRE(restored.from == edge.from);
        REQUIRE(restored.to == edge.to);
        REQUIRE(restored.type == LinkType::Implicit);
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
        index.add_file(FileEntry { .id = 1, .name = intern("foo.c"), .path = intern("foo.c") });
        index.add_file(FileEntry { .id = 2, .name = intern("bar.c"), .path = intern("bar.c") });

        REQUIRE(index.file_count() == 2);
        REQUIRE_FALSE(index.empty());

        auto* by_id_1 = index.find_file_by_id(1);
        REQUIRE(by_id_1 != nullptr);
        REQUIRE(by_id_1->name == intern("foo.c"));

        auto* by_id_2 = index.find_file_by_id(2);
        REQUIRE(by_id_2 != nullptr);
        REQUIRE(by_id_2->name == intern("bar.c"));

        REQUIRE(index.find_file_by_id(999) == nullptr);
    }

    SECTION("add and find commands")
    {
        index.add_command(CommandEntry { .id = node_id::make_command(1), .instruction_pattern = intern("gcc foo.c") });
        index.add_command(CommandEntry { .id = node_id::make_command(2), .instruction_pattern = intern("gcc bar.c") });

        REQUIRE(index.command_count() == 2);

        auto* found = index.find_command_by_id(node_id::make_command(1));
        REQUIRE(found != nullptr);
        REQUIRE(found->instruction_pattern == intern("gcc foo.c"));

        REQUIRE(index.find_command_by_id(node_id::make_command(999)) == nullptr);
    }

    SECTION("add and query edges")
    {
        auto cmd1 = node_id::make_command(1);
        auto cmd2 = node_id::make_command(2);
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
        auto cmd1 = node_id::make_command(1);
        index.add_file(FileEntry { .id = 1, .name = intern("test.c") });
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

TEST_CASE("Serialized edge section is independent of edge insertion order", "[index][determinism]")
{
    auto const cmd1 = node_id::make_command(1);
    auto const cmd2 = node_id::make_command(2);

    auto serialize_with_edges = [&](std::vector<EdgeEntry> const& edges) {
        auto index = Index {};
        index.add_file(FileEntry { .id = 1, .parent_id = 0, .type = NodeType::Directory, .name = intern("src") });
        index.add_file(FileEntry { .id = 2, .parent_id = 1, .type = NodeType::File, .name = intern("a.c") });
        index.add_file(FileEntry { .id = 3, .parent_id = 1, .type = NodeType::Generated, .name = intern("a.o") });
        index.add_file(FileEntry { .id = 4, .parent_id = 1, .type = NodeType::File, .name = intern("a.h") });
        index.add_file(FileEntry { .id = 5, .parent_id = 1, .type = NodeType::Generated, .name = intern("b.o") });
        index.add_command(CommandEntry {
            .id = cmd1,
            .dir_id = 1,
            .instruction_pattern = intern("cc -c %f -o %o"),
            .inputs = { 2 },
            .outputs = { 3 },
        });
        index.add_command(CommandEntry {
            .id = cmd2,
            .dir_id = 1,
            .instruction_pattern = intern("cc -S %f -o %o"),
            .inputs = { 2 },
            .outputs = { 5 },
        });
        for (auto const& e : edges) {
            index.add_edge(e);
        }
        auto data = serialize_index(index);
        REQUIRE(data.has_value());
        return std::move(*data);
    };

    auto edge_section = [](Vec<std::byte> const& data) {
        auto hdr = RawHeader {};
        std::memcpy(&hdr, data.data(), sizeof(hdr));
        auto const* begin = data.data() + hdr.edge_offset;
        return std::vector<std::byte> { begin, begin + hdr.edge_count * sizeof(RawEdge) };
    };

    // Ties on from (edges 4->cmd1 / 4->cmd2) and on (from, to) (Implicit vs
    // Sticky 4->cmd1) exercise every leg of the canonical-order comparator.
    auto edges = std::vector<EdgeEntry> {
        EdgeEntry { .from = 2, .to = cmd1, .type = LinkType::Normal },
        EdgeEntry { .from = cmd1, .to = 3, .type = LinkType::Normal },
        EdgeEntry { .from = 4, .to = cmd1, .type = LinkType::Implicit },
        EdgeEntry { .from = 4, .to = cmd1, .type = LinkType::Sticky },
        EdgeEntry { .from = 4, .to = cmd2, .type = LinkType::Implicit },
        EdgeEntry { .from = 2, .to = cmd2, .type = LinkType::Normal },
        EdgeEntry { .from = cmd2, .to = 5, .type = LinkType::Normal },
    };

    auto reference = edge_section(serialize_with_edges(edges));
    auto permuted = edges;
    std::reverse(permuted.begin(), permuted.end());
    REQUIRE(edge_section(serialize_with_edges(permuted)) == reference);

    std::rotate(permuted.begin(), permuted.begin() + 3, permuted.end());
    REQUIRE(edge_section(serialize_with_edges(permuted)) == reference);
}

TEST_CASE("Index serialization roundtrip", "[e2e][index]")
{
    // IDs must be consecutive and match array position (id = array_index + 1)
    // Files: 1, 2, 3, 4, 5 in insertion order
    // Commands: node_id::make_command(1)
    auto const cmd_id = node_id::make_command(1);

    auto index = Index {};

    // Add directories first (for parent chain)
    // File 1: src directory
    index.add_file(FileEntry {
        .id = 1,
        .parent_id = 0,
        .type = NodeType::Directory,
        .name = intern("src"),
    });

    // File 2: build directory
    index.add_file(FileEntry {
        .id = 2,
        .parent_id = 0,
        .type = NodeType::Directory,
        .name = intern("build"),
    });

    // File 3: main.cpp (parent is src, id=1)
    index.add_file(FileEntry {
        .id = 3,
        .parent_id = 1,
        .type = NodeType::File,
        .name = intern("main.cpp"),
        .size = 1024,
    });

    // File 4: main.o (parent is build, id=2)
    index.add_file(FileEntry {
        .id = 4,
        .parent_id = 2,
        .type = NodeType::Generated,
        .name = intern("main.o"),
        .size = 4096,
    });

    // File 5: header file (implicit dependency)
    index.add_file(FileEntry {
        .id = 5,
        .parent_id = 0,
        .type = NodeType::File,
        .name = intern("/usr/include/stdio.h"),
        .size = 8192,
    });

    // Command 1 (v8: template + operands; v19: + key and signature hashes)
    auto cmd_key = pup::Hash256 {};
    cmd_key[0] = std::byte { 0x11 };
    cmd_key[31] = std::byte { 0x99 };
    index.add_command(CommandEntry {
        .id = cmd_id,
        .dir_id = 0,
        .instruction_pattern = intern("g++ -c %f -o %o"),
        .display = intern("CXX main.cpp"),
        .env = {},
        .key = cmd_key,
        .inputs = { 3 },   // main.cpp
        .outputs = { 4 },  // main.o
    });

    // Add edges (file 3 -> cmd, cmd -> file 4, file 5 -> cmd implicit)
    index.add_edge(EdgeEntry { .from = 3, .to = cmd_id, .type = LinkType::Normal });
    index.add_edge(EdgeEntry { .from = cmd_id, .to = 4, .type = LinkType::Normal });
    index.add_edge(EdgeEntry { .from = 5, .to = cmd_id, .type = LinkType::Implicit });

    // Serialize
        auto data = serialize_index(index);
    REQUIRE(data.has_value());
    REQUIRE(data->size() > sizeof(RawHeader) + sizeof(RawFooter));

    // Write to temp file and read back
    auto temp_path = (std::filesystem::temp_directory_path() / "pup_test_index").string();

    auto write_result = write_index(temp_path, index);
    REQUIRE(write_result.has_value());

    // Verify file exists
    REQUIRE(std::filesystem::exists(temp_path));

    // Read back
    auto reader_result = open_index(temp_path);
    REQUIRE(reader_result.has_value());

    auto& idx_file = *reader_result;
    REQUIRE(index_is_open(idx_file));

    // Check header
    auto const* hdr = index_header(idx_file);
    REQUIRE(hdr != nullptr);
    REQUIRE(std::memcmp(hdr->magic.data(), INDEX_MAGIC.data(), 4) == 0);
    REQUIRE(hdr->version == INDEX_VERSION);
    REQUIRE(hdr->file_count == 5);
    REQUIRE(hdr->command_count == 1);
    REQUIRE(hdr->edge_count == 3);

    // Verify checksum
    REQUIRE(index_verify_checksum(idx_file));

    // Read full index
    auto read_result = read_index(idx_file);
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

    // Verify command (ID computed from position: node_id::make_command(0 + 1))
    auto* cmd = restored.find_command_by_id(cmd_id);
    REQUIRE(cmd != nullptr);
    REQUIRE(cmd->instruction_pattern == intern("g++ -c %f -o %o"));
    REQUIRE(cmd->display == intern("CXX main.cpp"));
    REQUIRE(cmd->key == cmd_key);
    REQUIRE(cmd->inputs == pup::Vec<NodeId> { 3 });
    REQUIRE(cmd->outputs == pup::Vec<NodeId> { 4 });

    // v8: Verify command reconstruction from template + operands
    auto reconstructed = get_command_string(restored, *cmd);
    REQUIRE(sv(reconstructed) =="g++ -c src/main.cpp -o build/main.o");

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

    idx_file.file.close();
    std::filesystem::remove(temp_path);
}

TEST_CASE("Index ID contiguity requirement", "[e2e][index]")
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
        .name = intern("src"),
    });

    index.add_file(FileEntry {
        .id = 2,
        .parent_id = 0,
        .type = NodeType::Directory,
        .name = intern("build"),
    });

    // ID 3: placeholder (like a Ghost node would be stored)
    index.add_file(FileEntry {
        .id = 3,
        .parent_id = 0,
        .type = NodeType::Ghost,
        .name = intern("placeholder"),
    });

    // ID 4: subdirectory under src (parent=1)
    index.add_file(FileEntry {
        .id = 4,
        .parent_id = 1,
        .type = NodeType::Directory,
        .name = intern("lib"),
    });

    // ID 5: file under lib (parent=4)
    index.add_file(FileEntry {
        .id = 5,
        .parent_id = 4,
        .type = NodeType::File,
        .name = intern("foo.c"),
        .size = 100,
    });

    // Serialize and read back
    auto temp_path = (std::filesystem::temp_directory_path() / "pup_test_contiguous").string();
        auto write_result = write_index(temp_path, index);
    REQUIRE(write_result.has_value());

    auto reader_result = open_index(temp_path);
    REQUIRE(reader_result.has_value());

    auto read_result = read_index(*reader_result);
    REQUIRE(read_result.has_value());

    auto& restored = *read_result;

    // With consecutive IDs, path reconstruction works correctly
    auto* foo = find_file_by_path(restored, "src/lib/foo.c");
    REQUIRE(foo != nullptr);
    REQUIRE(foo->size == 100);

    auto* lib = find_file_by_path(restored, "src/lib");
    REQUIRE(lib != nullptr);
    REQUIRE(lib->type == NodeType::Directory);

    reader_result->file.close();
    std::filesystem::remove(temp_path);
}

TEST_CASE("Index reader validation", "[e2e][index]")
{
    SECTION("non-existent file")
    {
        auto result = open_index("/nonexistent/path/to/index");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("is_valid_index")
    {
        REQUIRE_FALSE(is_valid_index("/nonexistent"));

        // Create a valid index
        auto index = Index {};
        index.add_file(FileEntry { .id = 1, .name = intern("test.c") });

        auto temp_path = (std::filesystem::temp_directory_path() / "pup_valid_test").string();
                (void)write_index(temp_path, index);

        REQUIRE(is_valid_index(temp_path));

        std::filesystem::remove(temp_path);
    }
}

TEST_CASE("A record whose declared layout does not fit the file is refused", "[e2e][index]")
{
    // Reading it as empty was the old behaviour: an out-of-bounds section became a zero-length
    // span, so a damaged record loaded as the record of a project that had produced nothing --
    // and the source-file guard, which reads exactly that, went quiet and let a rule overwrite
    // a checked-in file (#293).
    auto const cmd_id = node_id::make_command(1);
    auto index = Index {};
    index.add_file(FileEntry { .id = 1, .name = intern("test.c") });
    index.add_command(CommandEntry { .id = cmd_id, .instruction_pattern = intern("gcc test.c"), .inputs = { 1 } });
    index.add_edge(EdgeEntry { .from = 1, .to = cmd_id });

    auto const data = serialize_index(index);
    REQUIRE(data.has_value());

    auto const path = (std::filesystem::temp_directory_path() / "pup_malicious_test").string();
    auto const past_end = static_cast<std::uint32_t>(data->size()) + 1000;
    auto const overflowing_count = static_cast<std::uint32_t>(data->size() / sizeof(RawFileEntry)) + 100;

    auto const write_mutated = [&](auto&& mutate) {
        auto corrupted = *data;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        mutate(*reinterpret_cast<RawHeader*>(corrupted.data()));
        auto out = std::ofstream { path, std::ios::binary | std::ios::trunc };
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        out.write(reinterpret_cast<char const*>(corrupted.data()), static_cast<std::streamsize>(corrupted.size()));
    };

    SECTION("the unmutated record still opens")
    {
        write_mutated([](RawHeader&) {});
        auto opened = open_index(path);
        REQUIRE(opened.has_value());
        opened->file.close();
    }

    SECTION("a section starting or running past the end is refused")
    {
        auto const [name, mutate] = GENERATE_REF(table<char const*, std::function<void(RawHeader&)>>({
            { "file_offset", [&](RawHeader& h) { h.file_offset = past_end; } },
            { "file_count", [](RawHeader& h) { h.file_count = 0xFFFFFFFF; } },
            { "file_count just past the end", [&](RawHeader& h) { h.file_offset = sizeof(RawHeader); h.file_count = overflowing_count; } },
            { "command_offset", [&](RawHeader& h) { h.command_offset = past_end; } },
            { "command_count", [](RawHeader& h) { h.command_count = 0xFFFFFFFF; } },
            { "edge_offset", [&](RawHeader& h) { h.edge_offset = past_end; } },
            { "edge_count", [](RawHeader& h) { h.edge_count = 0xFFFFFFFF; } },
            { "string_offset", [&](RawHeader& h) { h.string_offset = past_end; } },
            { "string_table_size", [](RawHeader& h) { h.string_table_size = 0xFFFFFFFF; } },
            { "operand_table_offset", [&](RawHeader& h) { h.operand_table_offset = past_end; } },
            { "operand_data_offset", [&](RawHeader& h) { h.operand_data_offset = past_end; } },
        }));
        INFO("mutated " << name);

        write_mutated(mutate);

        auto opened = open_index(path);
        REQUIRE_FALSE(opened.has_value());
    }

    std::filesystem::remove(path);
}

TEST_CASE("StringTable overflow handling", "[index]")
{
    SECTION("file name exceeding 64KB limit fails serialization")
    {
        auto index = Index {};

        // Create a string larger than 64KB (0xFFFF = 65535 bytes max)
        auto huge_name = Buf {};
        huge_name.resize(65536);
        std::memset(huge_name.data(), 'x', 65536);

        index.add_file(FileEntry { .id = 1, .parent_id = 0, .name = global_pool().intern(huge_name.view()) });

                auto result = serialize_index(index);

        REQUIRE_FALSE(result.has_value());
        REQUIRE(global_pool().get(result.error().message).find("64KB") != std::string_view::npos);
    }

    SECTION("file name at 64KB limit succeeds")
    {
        auto index = Index {};

        // Create a string exactly at the 64KB limit (65535 bytes)
        auto max_name = Buf {};
        max_name.resize(65535);
        std::memset(max_name.data(), 'y', 65535);

        index.add_file(FileEntry { .id = 1, .parent_id = 0, .name = global_pool().intern(max_name.view()) });

                auto result = serialize_index(index);

        REQUIRE(result.has_value());
    }
}

TEST_CASE("A command longer than the string table's entry limit is still recorded", "[index]")
{
    auto oversized = Buf {};
    oversized.resize(70000);
    std::memset(oversized.data(), 'c', 70000);

    auto index = Index {};
    index.add_file(FileEntry { .id = 1, .parent_id = 0, .type = NodeType::Directory, .name = intern("dir") });
    auto cmd_id = node_id::make_command(1);
    index.add_command(CommandEntry {
        .id = cmd_id,
        .dir_id = 1,
        .instruction_pattern = intern(oversized.view()),
        .display = intern(oversized.view()),
    });

    auto temp_path = (std::filesystem::temp_directory_path() / "pup_oversized_command_test").string();
    REQUIRE(write_index(temp_path, index).has_value());

    auto opened = open_index(temp_path);
    REQUIRE(opened.has_value());
    auto restored = read_index(*opened);
    REQUIRE(restored.has_value());

    auto* cmd = restored->find_command_by_id(cmd_id);
    REQUIRE(cmd != nullptr);

    SECTION("a command exactly at the entry limit is recorded whole")
    {
        auto at_limit = Buf {};
        at_limit.resize(65535);
        std::memset(at_limit.data(), 'e', 65535);

        auto exact = Index {};
        exact.add_file(FileEntry { .id = 1, .parent_id = 0, .type = NodeType::Directory, .name = intern("dir") });
        exact.add_command(CommandEntry { .id = cmd_id, .dir_id = 1, .instruction_pattern = intern(at_limit.view()) });

        auto exact_path = (std::filesystem::temp_directory_path() / "pup_at_limit_command_test").string();
        REQUIRE(write_index(exact_path, exact).has_value());

        auto exact_opened = open_index(exact_path);
        REQUIRE(exact_opened.has_value());
        auto exact_restored = read_index(*exact_opened);
        REQUIRE(exact_restored.has_value());

        auto const* exact_cmd = exact_restored->find_command_by_id(cmd_id);
        REQUIRE(exact_cmd != nullptr);
        REQUIRE(sv(exact_cmd->instruction_pattern) == at_limit.view());

        exact_opened->file.close();
        std::filesystem::remove(exact_path);
    }

    SECTION("each display field is recorded, marked as truncated, and within the entry limit")
    {
        for (auto field : { cmd->instruction_pattern, cmd->display }) {
            auto text = sv(field);
            REQUIRE(text.size() <= 65535);
            REQUIRE(text.starts_with("cccc"));
            REQUIRE(text.find("truncated") != std::string_view::npos);
            REQUIRE(text.find("70000") != std::string_view::npos);
        }
    }

    opened->file.close();
    std::filesystem::remove(temp_path);
}

TEST_CASE("An environment longer than the string table's entry limit fails the record", "[index]")
{
    auto oversized = Buf {};
    oversized.resize(70000);
    std::memset(oversized.data(), 'v', 70000);

    auto index = Index {};
    index.add_file(FileEntry { .id = 1, .parent_id = 0, .type = NodeType::Directory, .name = intern("dir") });
    index.add_command(CommandEntry {
        .id = node_id::make_command(1),
        .dir_id = 1,
        .instruction_pattern = intern("cc %f"),
        .env = intern(oversized.view()),
    });

    auto result = serialize_index(index);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(global_pool().get(result.error().message).find("64KB") != std::string_view::npos);
}

TEST_CASE("StringTable deduplication", "[index]")
{
    SECTION("identical strings are deduplicated")
    {
        auto index = Index {};

        index.add_file(FileEntry { .id = 1, .parent_id = 0, .name = intern("main.cpp") });
        index.add_file(FileEntry { .id = 2, .parent_id = 0, .name = intern("main.cpp") });
        index.add_file(FileEntry { .id = 3, .parent_id = 0, .name = intern("main.cpp") });

                auto data = serialize_index(index);
        REQUIRE(data.has_value());

        // v6 length-prefixed: 2 (empty) + 2 (length) + 8 (main.cpp) = 12
        auto const* hdr = reinterpret_cast<RawHeader const*>(data->data());
        REQUIRE(hdr->string_table_size == 12);
    }

    SECTION("system header paths are deduplicated")
    {
        auto index = Index {};

        index.add_file(FileEntry { .id = 1, .parent_id = 0, .name = intern("/usr/include/stdio.h") });
        index.add_file(FileEntry { .id = 2, .parent_id = 0, .name = intern("/usr/include/stdio.h") });

                auto data = serialize_index(index);
        REQUIRE(data.has_value());

        // v6 length-prefixed: 2 (empty) + 2 (length) + 20 (path) = 24
        auto const* hdr = reinterpret_cast<RawHeader const*>(data->data());
        REQUIRE(hdr->string_table_size == 24);
    }

    SECTION("empty strings return offset 0")
    {
        auto index = Index {};

        index.add_command(CommandEntry { .id = node_id::make_command(1), .instruction_pattern = intern("gcc"), .display = intern(""), .env = intern("") });
        index.add_command(CommandEntry { .id = node_id::make_command(2), .instruction_pattern = intern("gcc"), .display = intern(""), .env = intern("") });

                auto data = serialize_index(index);
        REQUIRE(data.has_value());

        // v8 length-prefixed: 2 (empty) + 2 (length) + 3 (gcc) = 7
        auto const* hdr = reinterpret_cast<RawHeader const*>(data->data());
        REQUIRE(hdr->string_table_size == 7);
    }
}

TEST_CASE("v8 template reconstruction", "[index][v8]")
{
    auto index = Index {};

    // Create directory structure for path reconstruction
    index.add_file(FileEntry { .id = 1, .parent_id = 0, .type = NodeType::Directory, .name = intern("src") });
    index.add_file(FileEntry { .id = 2, .parent_id = 0, .type = NodeType::Directory, .name = intern("build") });
    index.add_file(FileEntry { .id = 3, .parent_id = 1, .type = NodeType::File, .name = intern("main.cpp") });
    index.add_file(FileEntry { .id = 4, .parent_id = 2, .type = NodeType::Generated, .name = intern("main.o") });

    // Compute paths from parent chain
    index.compute_paths();

    SECTION("get_command_string with %f and %o")
    {
        auto cmd = CommandEntry {
            .id = node_id::make_command(1),
            .instruction_pattern = intern("g++ -c %f -o %o"),
            .inputs = { 3 },   // main.cpp
            .outputs = { 4 },  // main.o
        };
        index.add_command(cmd);

        auto result = get_command_string(index, cmd);
        REQUIRE(sv(result) =="g++ -c src/main.cpp -o build/main.o");
    }

    SECTION("get_command_string with %b (basename)")
    {
        auto cmd = CommandEntry {
            .id = node_id::make_command(1),
            .instruction_pattern = intern("echo %b"),
            .inputs = { 3 },
            .outputs = {},
        };
        index.add_command(cmd);

        auto result = get_command_string(index, cmd);
        REQUIRE(sv(result) =="echo main.cpp");
    }

    SECTION("get_command_string with %B (basename without extension)")
    {
        auto cmd = CommandEntry {
            .id = node_id::make_command(1),
            .instruction_pattern = intern("echo %B"),
            .inputs = { 3 },
            .outputs = {},
        };
        index.add_command(cmd);

        auto result = get_command_string(index, cmd);
        REQUIRE(sv(result) =="echo main");
    }

    SECTION("get_command_string with %e (extension)")
    {
        auto cmd = CommandEntry {
            .id = node_id::make_command(1),
            .instruction_pattern = intern("echo %e"),
            .inputs = { 3 },
            .outputs = {},
        };
        index.add_command(cmd);

        auto result = get_command_string(index, cmd);
        REQUIRE(sv(result) =="echo cpp");
    }

    SECTION("get_command_string with multiple inputs")
    {
        index.add_file(FileEntry { .id = 5, .parent_id = 1, .type = NodeType::File, .name = intern("util.cpp") });
        index.compute_paths();

        auto cmd = CommandEntry {
            .id = node_id::make_command(1),
            .instruction_pattern = intern("g++ -c %f -o %o"),
            .inputs = { 3, 5 },
            .outputs = { 4 },
        };
        index.add_command(cmd);

        auto result = get_command_string(index, cmd);
        REQUIRE(sv(result) =="g++ -c src/main.cpp src/util.cpp -o build/main.o");
    }

    SECTION("get_command_string with %Nf (N-th input)")
    {
        index.add_file(FileEntry { .id = 5, .parent_id = 1, .type = NodeType::File, .name = intern("util.cpp") });
        index.compute_paths();

        auto cmd = CommandEntry {
            .id = node_id::make_command(1),
            .instruction_pattern = intern("echo %1f %2f"),
            .inputs = { 3, 5 },
            .outputs = {},
        };
        index.add_command(cmd);

        auto result = get_command_string(index, cmd);
        REQUIRE(sv(result) =="echo src/main.cpp src/util.cpp");
    }

    SECTION("get_command_string with %% escape")
    {
        auto cmd = CommandEntry {
            .id = node_id::make_command(1),
            .instruction_pattern = intern("echo 100%%"),
            .inputs = {},
            .outputs = {},
        };
        index.add_command(cmd);

        auto result = get_command_string(index, cmd);
        REQUIRE(sv(result) =="echo 100%");
    }

    SECTION("get_command_string with empty template")
    {
        auto cmd = CommandEntry {
            .id = node_id::make_command(1),
            .instruction_pattern = intern(""),
            .inputs = {},
            .outputs = {},
        };
        index.add_command(cmd);

        auto result = get_command_string(index, cmd);
        REQUIRE(pup::is_empty(result));
    }
}

TEST_CASE("v8 cross-directory path relativization", "[index][v8]")
{
    auto index = Index {};

    // Create directory structure:
    //   lib/foo.c, lib/foo.o
    //   app/main.c, app/app
    // Command runs from "app" directory, referencing "../lib/foo.o"
    index.add_file(FileEntry { .id = 1, .parent_id = 0, .type = NodeType::Directory, .name = intern("lib"), .path = intern("lib") });
    index.add_file(FileEntry { .id = 2, .parent_id = 0, .type = NodeType::Directory, .name = intern("app"), .path = intern("app") });
    index.add_file(FileEntry { .id = 3, .parent_id = 1, .type = NodeType::File, .name = intern("foo.c"), .path = intern("lib/foo.c") });
    index.add_file(FileEntry { .id = 4, .parent_id = 1, .type = NodeType::Generated, .name = intern("foo.o"), .path = intern("lib/foo.o") });
    index.add_file(FileEntry { .id = 5, .parent_id = 2, .type = NodeType::File, .name = intern("main.c"), .path = intern("app/main.c") });
    index.add_file(FileEntry { .id = 6, .parent_id = 2, .type = NodeType::Generated, .name = intern("app"), .path = intern("app/app") });

    SECTION("command in subdirectory referencing sibling directory")
    {
        // Command runs from "app" directory, links with ../lib/foo.o
        auto cmd = CommandEntry {
            .id = node_id::make_command(1),
            .dir_id = 2,  // app directory
            .instruction_pattern = intern("gcc %f -o %o"),
            .inputs = { 5, 4 },   // main.c, foo.o (from lib)
            .outputs = { 6 },      // app
        };
        index.add_command(cmd);

        auto result = get_command_string(index, cmd);
        // Paths should be relative to "app" directory
        REQUIRE(sv(result) =="gcc main.c ../lib/foo.o -o app");
    }

    SECTION("command in root referencing subdirectory files")
    {
        // Command runs from root, uses full paths
        auto cmd = CommandEntry {
            .id = node_id::make_command(1),
            .dir_id = 0,  // root directory (no relativization)
            .instruction_pattern = intern("gcc %f -o %o"),
            .inputs = { 5, 4 },
            .outputs = { 6 },
        };
        index.add_command(cmd);

        auto result = get_command_string(index, cmd);
        // Paths should be full since dir_id is 0
        REQUIRE(sv(result) =="gcc app/main.c lib/foo.o -o app/app");
    }

    SECTION("command in subdirectory with same-directory files")
    {
        // Command runs from "lib" directory, all files are local
        auto cmd = CommandEntry {
            .id = node_id::make_command(1),
            .dir_id = 1,  // lib directory
            .instruction_pattern = intern("gcc -c %f -o %o"),
            .inputs = { 3 },   // foo.c
            .outputs = { 4 },  // foo.o
        };
        index.add_command(cmd);

        auto result = get_command_string(index, cmd);
        // Paths should be relative to "lib" directory
        REQUIRE(sv(result) =="gcc -c foo.c -o foo.o");
    }
}

TEST_CASE("v8 build_command_lookup", "[index][v8]")
{
    auto index = Index {};

    // Create file entries
    index.add_file(FileEntry { .id = 1, .parent_id = 0, .type = NodeType::File, .name = intern("foo.c") });
    index.add_file(FileEntry { .id = 2, .parent_id = 0, .type = NodeType::Generated, .name = intern("foo.o") });
    index.add_file(FileEntry { .id = 3, .parent_id = 0, .type = NodeType::File, .name = intern("bar.c") });
    index.add_file(FileEntry { .id = 4, .parent_id = 0, .type = NodeType::Generated, .name = intern("bar.o") });
    index.compute_paths();

    // Add commands with template + operands
    index.add_command(CommandEntry {
        .id = node_id::make_command(1),
        .instruction_pattern = intern("gcc -c %f -o %o"),
        .inputs = { 1 },
        .outputs = { 2 },
    });
    index.add_command(CommandEntry {
        .id = node_id::make_command(2),
        .instruction_pattern = intern("gcc -c %f -o %o"),
        .inputs = { 3 },
        .outputs = { 4 },
    });

    SECTION("lookup by reconstructed command string")
    {
        auto lookup = build_command_lookup(index);

        REQUIRE(lookup.size() == 2);
        REQUIRE(lookup.contains("gcc -c foo.c -o foo.o"));
        REQUIRE(lookup.contains("gcc -c bar.c -o bar.o"));

        auto const* cmd1 = lookup.at("gcc -c foo.c -o foo.o");
        REQUIRE(cmd1 != nullptr);
        REQUIRE(cmd1->id == node_id::make_command(1));

        auto const* cmd2 = lookup.at("gcc -c bar.c -o bar.o");
        REQUIRE(cmd2 != nullptr);
        REQUIRE(cmd2->id == node_id::make_command(2));
    }

    SECTION("same template, different operands")
    {
        auto lookup = build_command_lookup(index);

        // Both commands have same template but different operands
        // So they should be distinct in the lookup
        REQUIRE(lookup.size() == 2);

        auto const* cmd1 = lookup.at("gcc -c foo.c -o foo.o");
        auto const* cmd2 = lookup.at("gcc -c bar.c -o bar.o");

        REQUIRE(cmd1->instruction_pattern == cmd2->instruction_pattern);
        REQUIRE(cmd1->inputs != cmd2->inputs);
    }
}

TEST_CASE("v8 roundtrip with operand sections", "[e2e][index][v8]")
{
    auto index = Index {};

    // Create file structure
    index.add_file(FileEntry { .id = 1, .parent_id = 0, .type = NodeType::Directory, .name = intern("src") });
    index.add_file(FileEntry { .id = 2, .parent_id = 0, .type = NodeType::Directory, .name = intern("build") });
    index.add_file(FileEntry { .id = 3, .parent_id = 1, .type = NodeType::File, .name = intern("main.cpp") });
    index.add_file(FileEntry { .id = 4, .parent_id = 1, .type = NodeType::File, .name = intern("util.cpp") });
    index.add_file(FileEntry { .id = 5, .parent_id = 2, .type = NodeType::Generated, .name = intern("main.o") });
    index.add_file(FileEntry { .id = 6, .parent_id = 2, .type = NodeType::Generated, .name = intern("util.o") });
    index.add_file(FileEntry { .id = 7, .parent_id = 2, .type = NodeType::Generated, .name = intern("program") });

    auto cmd1_id = node_id::make_command(1);
    auto cmd2_id = node_id::make_command(2);
    auto cmd3_id = node_id::make_command(3);

    // Commands with templates + operands
    index.add_command(CommandEntry {
        .id = cmd1_id,
        .instruction_pattern = intern("g++ -c %f -o %o"),
        .display = intern("CXX main.cpp"),
        .inputs = { 3 },
        .outputs = { 5 },
    });
    index.add_command(CommandEntry {
        .id = cmd2_id,
        .instruction_pattern = intern("g++ -c %f -o %o"),
        .display = intern("CXX util.cpp"),
        .inputs = { 4 },
        .outputs = { 6 },
    });
    index.add_command(CommandEntry {
        .id = cmd3_id,
        .instruction_pattern = intern("g++ %f -o %o"),
        .display = intern("LD program"),
        .inputs = { 5, 6 },
        .outputs = { 7 },
    });

    // Serialize
    auto data = serialize_index(index);
    REQUIRE(data.has_value());

    // Write and read back
    auto temp_path = (std::filesystem::temp_directory_path() / "pup_v8_roundtrip_test").string();
    auto write_result = write_index(temp_path, index);
    REQUIRE(write_result.has_value());

    auto reader_result = open_index(temp_path);
    REQUIRE(reader_result.has_value());

    auto read_result = read_index(*reader_result);
    REQUIRE(read_result.has_value());

    auto& restored = *read_result;

    // Verify structure
    REQUIRE(restored.file_count() == 7);
    REQUIRE(restored.command_count() == 3);

    // Verify commands retain template + operands
    auto* cmd1 = restored.find_command_by_id(cmd1_id);
    REQUIRE(cmd1 != nullptr);
    REQUIRE(cmd1->instruction_pattern == intern("g++ -c %f -o %o"));
    REQUIRE(cmd1->inputs == pup::Vec<NodeId> { 3 });
    REQUIRE(cmd1->outputs == pup::Vec<NodeId> { 5 });

    auto* cmd2 = restored.find_command_by_id(cmd2_id);
    REQUIRE(cmd2 != nullptr);
    REQUIRE(cmd2->instruction_pattern == intern("g++ -c %f -o %o"));
    REQUIRE(cmd2->inputs == pup::Vec<NodeId> { 4 });
    REQUIRE(cmd2->outputs == pup::Vec<NodeId> { 6 });

    auto* cmd3 = restored.find_command_by_id(cmd3_id);
    REQUIRE(cmd3 != nullptr);
    REQUIRE(cmd3->instruction_pattern == intern("g++ %f -o %o"));
    REQUIRE(cmd3->inputs == pup::Vec<NodeId> { 5, 6 });
    REQUIRE(cmd3->outputs == pup::Vec<NodeId> { 7 });

    // Verify command reconstruction
    auto cmd1_str = get_command_string(restored, *cmd1);
    REQUIRE(sv(cmd1_str) =="g++ -c src/main.cpp -o build/main.o");

    auto cmd2_str = get_command_string(restored, *cmd2);
    REQUIRE(sv(cmd2_str) =="g++ -c src/util.cpp -o build/util.o");

    auto cmd3_str = get_command_string(restored, *cmd3);
    REQUIRE(sv(cmd3_str) =="g++ build/main.o build/util.o -o build/program");

    reader_result->file.close();
    std::filesystem::remove(temp_path);
}

namespace {

auto node_label(pup::graph::Graph const& g, pup::NodeId id) -> std::string
{
    if (node_id::is_command(id)) {
        return "C:" + std::string(sv(pup::graph::get<pup::graph::InstructionPattern>(g, id)));
    }
    auto type = pup::graph::get<NodeType>(g, id);
    return "F" + std::to_string(static_cast<int>(type)) + ":"
        + std::string(sv(pup::graph::get<pup::graph::Name>(g, id)));
}

auto entry_label(Index const& idx, pup::NodeId id) -> std::string
{
    if (node_id::is_command(id)) {
        auto const* cmd = idx.find_command_by_id(id);
        return cmd ? "C:" + std::string(sv(cmd->instruction_pattern)) : "C:<missing>";
    }
    auto const* file = idx.find_file_by_id(id);
    return file
        ? "F" + std::to_string(static_cast<int>(file->type)) + ":" + std::string(sv(file->name))
        : "F:<missing>";
}

auto sorted_edge_labels_of_graph(pup::graph::Graph const& g) -> std::vector<std::string>
{
    auto touches_inactive_command = [&g](pup::NodeId id) {
        return node_id::is_command(id) && !pup::graph::is_guard_satisfied(g, id);
    };
    auto labels = std::vector<std::string> {};
    for (auto const& edge : g.edges) {
        if (edge.type == LinkType::OrderOnly) {
            continue;
        }
        if (touches_inactive_command(edge.from) || touches_inactive_command(edge.to)) {
            continue;
        }
        labels.push_back(
            node_label(g, edge.from) + "|" + std::to_string(static_cast<int>(edge.type))
            + "|" + node_label(g, edge.to)
        );
    }
    std::sort(labels.begin(), labels.end());
    return labels;
}

auto sorted_edge_labels_of_index(Index const& idx) -> std::vector<std::string>
{
    auto labels = std::vector<std::string> {};
    for (auto const& edge : idx.edges()) {
        labels.push_back(
            entry_label(idx, edge.from) + "|" + std::to_string(static_cast<int>(edge.type))
            + "|" + entry_label(idx, edge.to)
        );
    }
    std::sort(labels.begin(), labels.end());
    return labels;
}

auto require_graph_index_roundtrip(pup::graph::BuildGraph const& bs, std::string_view file_tag) -> void
{
    auto [index, path_to_id] = pup::cli::serialize_graph_nodes(bs, ".", ".", ".");
    auto cmd_remap = pup::cli::serialize_command_nodes(bs, index, path_to_id);
    pup::cli::serialize_edges(bs, index, cmd_remap);

    auto live_file_nodes = std::size_t { 0 };
    auto active_commands = std::size_t { 0 };
    for (auto id : pup::graph::all_nodes(bs.graph)) {
        if (!node_id::is_command(id)) {
            ++live_file_nodes;
        } else if (pup::graph::is_guard_satisfied(bs.graph, id)) {
            ++active_commands;
        }
    }

    auto temp_path = (std::filesystem::temp_directory_path() / file_tag).string();
    auto write_result = write_index(temp_path, index);
    REQUIRE(write_result.has_value());

    auto loaded = read_index(temp_path);
    std::filesystem::remove(temp_path);
    REQUIRE(loaded.has_value());

    REQUIRE(loaded->file_count() == live_file_nodes);
    REQUIRE(loaded->command_count() == active_commands);
    REQUIRE(sorted_edge_labels_of_index(*loaded) == sorted_edge_labels_of_graph(bs.graph));
}

} // namespace

TEST_CASE("serialize_index rejects a non-dense file id sequence", "[index]")
{
    auto index = Index {};
    index.add_file(FileEntry { .id = 1, .parent_id = 0, .type = NodeType::Directory, .name = intern("src") });
    index.add_file(FileEntry { .id = 3, .parent_id = 1, .type = NodeType::File, .name = intern("a.c") });

    REQUIRE_FALSE(serialize_index(index).has_value());
}

TEST_CASE("serialize_index rejects a non-dense command id sequence", "[index]")
{
    auto index = Index {};
    index.add_file(FileEntry { .id = 1, .parent_id = 0, .type = NodeType::File, .name = intern("a.c") });
    index.add_command(CommandEntry {
        .id = node_id::make_command(2),
        .dir_id = 0,
        .instruction_pattern = intern("cc a.c"),
    });

    REQUIRE_FALSE(serialize_index(index).has_value());
}

TEST_CASE("graph-to-index roundtrip keeps every node and edge endpoint", "[index]")
{
    auto bs = pup::graph::make_build_graph();
    auto& g = bs.graph;

    auto root = pup::graph::add_file_node(g, pup::graph::FileNode { .type = NodeType::Root, .name = intern("prj_root") });
    auto nameless = pup::graph::add_file_node(g, pup::graph::FileNode { .type = NodeType::File, .name = pup::StringId::Empty });
    auto src = pup::graph::add_file_node(g, pup::graph::FileNode { .type = NodeType::File, .name = intern("a.c") });
    auto var = pup::graph::add_file_node(g, pup::graph::FileNode { .type = NodeType::Variable, .name = intern("CC") });
    auto group = pup::graph::add_file_node(g, pup::graph::FileNode { .type = NodeType::Group, .name = intern("objs") });
    auto ghost = pup::graph::add_file_node(g, pup::graph::FileNode { .type = NodeType::Ghost, .name = intern("missing.h") });
    auto out = pup::graph::add_file_node(g, pup::graph::FileNode { .type = NodeType::Generated, .name = intern("a.o") });
    auto cmd = pup::graph::add_command_node(g, pup::graph::CommandNode { .instruction_id = intern("cc a.c") });

    REQUIRE(root.has_value());
    REQUIRE(nameless.has_value());
    REQUIRE(src.has_value());
    REQUIRE(var.has_value());
    REQUIRE(group.has_value());
    REQUIRE(ghost.has_value());
    REQUIRE(out.has_value());
    REQUIRE(cmd.has_value());

    REQUIRE(pup::graph::add_edge(g, *src, *cmd).has_value());
    REQUIRE(pup::graph::add_edge(g, *ghost, *cmd).has_value());
    REQUIRE(pup::graph::add_edge(g, *var, *cmd, LinkType::Sticky).has_value());
    REQUIRE(pup::graph::add_edge(g, *cmd, *out).has_value());
    REQUIRE(pup::graph::add_edge(g, *out, *group, LinkType::Group).has_value());

    require_graph_index_roundtrip(bs, "pup_test_rt_types");
}

TEST_CASE("graph-to-index roundtrip holds for randomized graphs", "[index]")
{
    auto const types = std::array {
        NodeType::File,
        NodeType::Generated,
        NodeType::Directory,
        NodeType::Variable,
        NodeType::Group,
        NodeType::Ghost,
        NodeType::GeneratedDir,
        NodeType::Root,
    };
    auto const link_types = std::array {
        LinkType::Normal,
        LinkType::Sticky,
        LinkType::Group,
        LinkType::Implicit,
    };

    for (auto seed = std::uint32_t { 0 }; seed < 20; ++seed) {
        INFO("seed=" << seed);
        auto rng = std::mt19937 { seed };
        auto bs = pup::graph::make_build_graph();
        auto& g = bs.graph;

        auto file_ids = std::vector<pup::NodeId> {};
        auto node_count = std::size_t { 3 + rng() % 20 };
        for (auto i = std::size_t { 0 }; i < node_count; ++i) {
            auto type = types[rng() % types.size()];
            auto name = (rng() % 8 == 0) ? pup::StringId::Empty : intern("n" + std::to_string(i));
            auto id = pup::graph::add_file_node(g, pup::graph::FileNode { .type = type, .name = name });
            REQUIRE(id.has_value());
            file_ids.push_back(*id);
        }

        auto false_cond = pup::graph::add_condition_node(g, pup::graph::ConditionNode { .expression = intern("cond_off"), .current_value = false });
        auto true_cond = pup::graph::add_condition_node(g, pup::graph::ConditionNode { .expression = intern("cond_on"), .current_value = true });
        REQUIRE(false_cond.has_value());
        REQUIRE(true_cond.has_value());

        auto cmd_ids = std::vector<pup::NodeId> {};
        auto cmd_count = std::size_t { 1 + rng() % 4 };
        for (auto i = std::size_t { 0 }; i < cmd_count; ++i) {
            auto guards = pup::Vec<pup::graph::Guard> {};
            switch (rng() % 4) {
            case 0:
                guards.push_back(pup::graph::Guard { .condition = *false_cond, .polarity = true });
                break;
            case 1:
                guards.push_back(pup::graph::Guard { .condition = *true_cond, .polarity = true });
                break;
            default:
                break;
            }
            auto id = pup::graph::add_command_node(g, pup::graph::CommandNode { .instruction_id = intern("cmd" + std::to_string(i)), .guards = std::move(guards) });
            REQUIRE(id.has_value());
            cmd_ids.push_back(*id);
        }

        auto edge_count = std::size_t { rng() % 30 };
        for (auto i = std::size_t { 0 }; i < edge_count; ++i) {
            auto from = file_ids[rng() % file_ids.size()];
            auto to = cmd_ids[rng() % cmd_ids.size()];
            if (rng() % 2 == 0) {
                std::swap(from, to);
            }
            auto type = link_types[rng() % link_types.size()];
            REQUIRE(pup::graph::add_edge(g, from, to, type).has_value());
        }

        require_graph_index_roundtrip(bs, "pup_test_rt_prop");
    }
}

namespace {

/// Rewrite a serialized record's version and re-sign it, the way an older putup would have
/// written it: every version since INDEX_LAYOUT_FLOOR lays its header and file table out
/// identically, so these bytes are what one of those really produced.
auto stamp_version(Vec<std::byte>& bytes, std::uint32_t version) -> void
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* hdr = reinterpret_cast<RawHeader*>(bytes.data());
    hdr->version = version;

    auto const content_size = bytes.size() - sizeof(RawFooter);
    auto const checksum = pup::sha256(std::span<std::byte const> { bytes.data(), content_size });
    std::memcpy(bytes.data() + content_size, checksum.data(), checksum.size());
}

auto write_bytes(std::string const& path, Vec<std::byte> const& bytes) -> void
{
    auto out = std::ofstream { path, std::ios::binary | std::ios::trunc };
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

auto temp_index_path(std::string_view stem) -> std::string
{
    return (std::filesystem::temp_directory_path() / std::string { stem }).string();
}

/// A record with one file of every type, laid out under a directory chain so the recovered
/// paths exercise the parent walk rather than bare basenames.
auto index_with_every_node_type() -> Index
{
    auto index = Index {};
    index.add_file(FileEntry { .id = 1, .type = NodeType::Directory, .name = intern("src") });
    index.add_file(FileEntry { .id = 2, .parent_id = 1, .type = NodeType::File, .name = intern("main.cpp") });
    index.add_file(FileEntry { .id = 3, .parent_id = 1, .type = NodeType::Generated, .name = intern("main.o") });
    index.add_file(FileEntry { .id = 4, .type = NodeType::Ghost, .name = intern("missing.h") });
    index.add_file(FileEntry { .id = 5, .type = NodeType::Group, .name = intern("objs") });
    index.add_file(FileEntry { .id = 6, .type = NodeType::File, .name = intern("Tupfile") });
    index.add_file(FileEntry { .id = 7, .parent_id = 1, .type = NodeType::Generated, .name = intern("gen.h") });
    index.compute_paths();
    return index;
}

/// A record of random shape: every node type, chains that terminate because a parent is always
/// an earlier entry, and empty names so entries without a path occur too.
auto random_index(std::uint32_t seed) -> Index
{
    auto const types = std::array {
        NodeType::File, NodeType::Generated, NodeType::Directory, NodeType::Variable,
        NodeType::Group, NodeType::Ghost, NodeType::GeneratedDir, NodeType::Root,
    };

    auto rng = std::mt19937 { seed };
    auto index = Index {};
    auto const count = std::size_t { 1 + rng() % 30 };
    for (auto i = std::size_t { 0 }; i < count; ++i) {
        auto parent = (i == 0 || rng() % 3 == 0) ? NodeId { 0 } : static_cast<NodeId>(1 + rng() % i);
        index.add_file(FileEntry {
            .id = static_cast<NodeId>(i + 1),
            .parent_id = parent,
            .type = types[rng() % types.size()],
            .name = (rng() % 8 == 0) ? StringId::Empty : intern("n" + std::to_string(rng() % 100)),
        });
    }
    index.compute_paths();
    return index;
}

auto sorted_paths_of_type(Index const& index, NodeType type) -> Vec<StringId>
{
    auto out = Vec<StringId> {};
    for (auto const& file : index.files()) {
        if (file.type == type && !pup::is_empty(file.path)) {
            out.push_back(file.path);
        }
    }
    std::sort(out.begin(), out.end(), pup::handle_less);
    return out;
}

} // namespace

TEST_CASE("A record names the paths it recorded even at a version too old to trust", "[index]")
{
    auto const index = index_with_every_node_type();
    auto const expected_sources = sorted_paths_of_type(index, NodeType::File);
    auto const expected_generated = sorted_paths_of_type(index, NodeType::Generated);
    REQUIRE_FALSE(expected_sources.empty());
    REQUIRE_FALSE(expected_generated.empty());

    auto const data = serialize_index(index);
    REQUIRE(data.has_value());
    auto const path = temp_index_path("pup_prior_window");

    // The window is a handful of integers, so it is enumerated rather than sampled.
    for (auto version = INDEX_LAYOUT_FLOOR - 1; version <= INDEX_VERSION + 1; ++version) {
        INFO("version=" << version);
        auto bytes = *data;
        stamp_version(bytes, version);
        write_bytes(path, bytes);

        auto const recovered = read_prior_paths(path);
        auto const in_window = version >= INDEX_LAYOUT_FLOOR && version <= INDEX_VERSION;

        if (in_window) {
            REQUIRE(recovered.kind == PriorPaths::Kind::Known);
            REQUIRE(recovered.sources == expected_sources);
            REQUIRE(recovered.generated == expected_generated);
        } else {
            REQUIRE(recovered.kind == PriorPaths::Kind::Lost);
            REQUIRE(recovered.sources.empty());
            REQUIRE(recovered.generated.empty());
        }
    }

    std::filesystem::remove(path);
}

TEST_CASE("Absence of a record is not the same answer as an unreadable one", "[index]")
{
    auto const missing = read_prior_paths(temp_index_path("pup_prior_absent"));
    REQUIRE(missing.kind == PriorPaths::Kind::NeverBuilt);

    auto const path = temp_index_path("pup_prior_garbage");
    auto garbage = Vec<std::byte> {};
    garbage.resize(200);
    write_bytes(path, garbage);
    REQUIRE(read_prior_paths(path).kind == PriorPaths::Kind::Lost);

    std::filesystem::remove(path);
}

TEST_CASE("Recovering a record's paths agrees with reading the record", "[index]")
{
    for (auto seed = std::uint32_t { 0 }; seed < 20; ++seed) {
        INFO("seed=" << seed);
        auto const index = random_index(seed);

        auto const path = temp_index_path("pup_prior_diff_" + std::to_string(seed));
        REQUIRE(write_index(path, index).has_value());

        auto const loaded = read_index(path);
        REQUIRE(loaded.has_value());

        auto const from_loaded = prior_paths(*loaded);
        auto const from_file = read_prior_paths(path);

        REQUIRE(from_loaded.kind == PriorPaths::Kind::Known);
        REQUIRE(from_file.kind == PriorPaths::Kind::Known);
        REQUIRE(from_file.sources == from_loaded.sources);
        REQUIRE(from_file.generated == from_loaded.generated);
        REQUIRE(from_file.sources == sorted_paths_of_type(index, NodeType::File));
        REQUIRE(from_file.generated == sorted_paths_of_type(index, NodeType::Generated));

        std::filesystem::remove(path);
    }
}

TEST_CASE("One flipped bit anywhere in a record makes it unrecoverable, never partly read", "[index]")
{
    auto const index = index_with_every_node_type();
    auto const data = serialize_index(index);
    REQUIRE(data.has_value());
    auto const path = temp_index_path("pup_prior_flip");

    auto rng = std::mt19937 { 20260804 };
    for (auto pos = std::size_t { 0 }; pos < data->size(); ++pos) {
        INFO("byte=" << pos);
        auto bytes = *data;
        auto const bit = std::uint8_t { 1 } << (rng() % 8);
        bytes[pos] = static_cast<std::byte>(static_cast<std::uint8_t>(bytes[pos]) ^ bit);
        write_bytes(path, bytes);

        auto const recovered = read_prior_paths(path);
        REQUIRE(recovered.kind == PriorPaths::Kind::Lost);
        REQUIRE(recovered.sources.empty());
        REQUIRE(recovered.generated.empty());
    }

    std::filesystem::remove(path);
}

// The mirror of the recovery-path property above, for the path every build takes: a record that
// is not the one putup wrote is not evidence, whichever door it is read through (#294).
TEST_CASE("One flipped bit anywhere in a record makes it unreadable", "[index]")
{
    auto const index = index_with_every_node_type();
    auto const data = serialize_index(index);
    REQUIRE(data.has_value());
    auto const path = temp_index_path("pup_read_flip");

    auto rng = std::mt19937 { 20260805 };
    for (auto pos = std::size_t { 0 }; pos < data->size(); ++pos) {
        INFO("byte=" << pos);
        auto bytes = *data;
        auto const bit = std::uint8_t { 1 } << (rng() % 8);
        bytes[pos] = static_cast<std::byte>(static_cast<std::uint8_t>(bytes[pos]) ^ bit);
        write_bytes(path, bytes);

        REQUIRE_FALSE(read_index(path).has_value());
    }

    std::filesystem::remove(path);
}

TEST_CASE("Keying a record's file table by path keeps every addressable entry and only those", "[index]")
{
    for (auto seed = std::uint32_t { 0 }; seed < 20; ++seed) {
        INFO("seed=" << seed);
        auto const index = random_index(seed);
        auto const by_path = files_by_path(index);

        auto expected = std::vector<FileEntry const*> {};
        for (auto const& file : index.files()) {
            if (!pup::is_empty(file.path)) {
                expected.push_back(&file);
            }
        }

        auto keyed = std::vector<FileEntry const*> {};
        for (auto const& [path, entry] : by_path.entries) {
            REQUIRE_FALSE(pup::is_empty(path));
            REQUIRE(entry != nullptr);
            REQUIRE(entry->path == path);
            keyed.push_back(entry);
        }

        // Same entries, each once: an index may record two nodes at one path, and dropping
        // either would make the map disagree with the table it is derived from.
        std::sort(expected.begin(), expected.end());
        std::sort(keyed.begin(), keyed.end());
        REQUIRE(keyed == expected);

        REQUIRE(std::is_sorted(by_path.entries.begin(), by_path.entries.end(), [](auto const& a, auto const& b) { return pup::handle_less(a.first, b.first); }));

        for (auto const* file : expected) {
            auto const* found = by_path.find(file->path);
            REQUIRE(found != nullptr);
            REQUIRE(found->path == file->path);
        }

        REQUIRE(by_path.find(intern("no entry is ever recorded at this path " + std::to_string(seed))) == nullptr);
        REQUIRE(by_path.find(StringId::Empty) == nullptr);
    }
}

TEST_CASE("Keying the index under construction by path answers the same however it was filled", "[index]")
{
    for (auto seed = std::uint32_t { 0 }; seed < 20; ++seed) {
        INFO("seed=" << seed);
        auto rng = std::mt19937 { seed };

        auto paths = std::vector<StringId> {};
        for (auto i = std::size_t { 0 }; i < 50; ++i) {
            paths.push_back(intern("seed" + std::to_string(seed) + "/dir" + std::to_string(rng() % 7) + "/f" + std::to_string(i) + ".c"));
        }
        std::shuffle(paths.begin(), paths.end(), rng);

        auto map = pup::cli::PathIdMap {};
        for (auto i = std::size_t { 0 }; i < paths.size(); ++i) {
            map.insert(paths[i], NodeId { static_cast<std::uint32_t>(i + 1) });
        }

        // The walks that build this map also query it, so what it answers cannot depend on how
        // far along the filling was — the insertion order is the graph's, not the caller's choice.
        for (auto i = std::size_t { 0 }; i < paths.size(); ++i) {
            auto found = map.find(paths[i]);
            REQUIRE(found.has_value());
            REQUIRE(*found == NodeId { static_cast<std::uint32_t>(i + 1) });
        }

        REQUIRE_FALSE(map.find(intern("seed" + std::to_string(seed) + "/never/inserted.c")).has_value());
        REQUIRE_FALSE(map.find(StringId::Empty).has_value());

        REQUIRE(std::is_sorted(map.entries.begin(), map.entries.end(), [](auto const& a, auto const& b) { return pup::handle_less(a.first, b.first); }));
        REQUIRE(map.entries.size() == paths.size());

        map.insert(paths[3], NodeId { 999 });
        REQUIRE(map.find(paths[3]) == NodeId { 999 });
        REQUIRE(map.entries.size() == paths.size());
    }
}
