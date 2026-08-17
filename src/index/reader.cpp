// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/index/reader.hpp"
#include "pup/core/expected.hpp"
#include "pup/core/hash.hpp"
#include "pup/core/result.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/index/entry.hpp"
#include "pup/index/format.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

namespace pup::index {

namespace {

/// Not the writer's truncation marker: that one states an event this read did not observe (#381).
constexpr auto UNREADABLE_DISPLAY = std::string_view { "[unreadable]" };

auto index_get_semantic_string(IndexFile const& f, std::uint32_t offset) -> Result<std::string_view>;
auto index_get_string(IndexFile const& f, std::uint32_t offset) -> std::string_view;
auto index_get_operands(IndexFile const& f, std::size_t cmd_index) -> Result<std::pair<Vec<NodeId>, Vec<NodeId>>>;

template<typename T>
auto read_raw_entries(
    pup::platform::MappedFile const& file,
    RawHeader const* hdr,
    std::uint32_t count,
    std::uint32_t offset
) -> std::span<T const>
{
    if (!hdr || count == 0) {
        return {};
    }

    auto const size = std::size_t { count } * sizeof(T);
    if (offset > file.size() || size > file.size() - offset) {
        return {};
    }

    auto data = std::span<std::byte const> { file.data(), file.size() };
    auto bytes = data.subspan(offset, size);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto const* entries = reinterpret_cast<T const*>(bytes.data());
    return { entries, count };
}

/// Every section the header declares has to lie inside the file and ahead of the footer. Without
/// this the out-of-range spans above come back empty instead of failing, so a damaged record
/// loads as the record of a project that produced nothing -- and the source-file guard, which
/// reads exactly that, goes quiet (#293).
auto declared_layout_fits(std::size_t file_size, RawHeader const& hdr) -> bool
{
    if (file_size < sizeof(RawHeader) + sizeof(RawFooter)) {
        return false;
    }
    auto const content_end = std::uint64_t { file_size } - sizeof(RawFooter);

    auto fits = [content_end](std::uint64_t offset, std::uint64_t size) {
        return offset <= content_end && size <= content_end - offset;
    };

    return fits(hdr.file_offset, std::uint64_t { hdr.file_count } * sizeof(RawFileEntry))
        && fits(hdr.command_offset, std::uint64_t { hdr.command_count } * sizeof(RawCommandEntry))
        && fits(hdr.edge_offset, std::uint64_t { hdr.edge_count } * sizeof(RawEdge))
        && fits(hdr.operand_table_offset, std::uint64_t { hdr.command_count } * sizeof(std::uint32_t))
        && fits(hdr.string_offset, hdr.string_table_size)
        // Operand data is the only section whose size the header does not carry: it runs from
        // its own offset to the string table.
        && hdr.operand_data_offset <= hdr.string_offset;
}

/// The readable window is always `[min_version, INDEX_VERSION]`. It never opens upward: a record
/// a newer putup wrote may lay its entries out in ways this one has no way to know about.
auto open_index_in_window(std::string_view path, std::uint32_t min_version) -> Result<IndexFile>
{
    auto result = IndexFile {};

    auto file_result = pup::platform::MappedFile::open(path);
    if (!file_result) {
        return make_error<IndexFile>(ErrorCode::IoError, "Failed to open index file");
    }

    result.file = std::move(*file_result);

    // Damage before version: neither a sub-header file nor foreign magic is a record of any version (#383).
    if (result.file.size() < sizeof(RawHeader) + sizeof(RawFooter)) {
        return make_error<IndexFile>(ErrorCode::IndexDamaged, "Index file too small");
    }

    auto const* hdr = index_header(result);
    if (!hdr || std::memcmp(hdr->magic.data(), INDEX_MAGIC.data(), 4) != 0) {
        return make_error<IndexFile>(ErrorCode::IndexDamaged, "Invalid index file magic");
    }
    if (hdr->version < min_version || hdr->version > INDEX_VERSION) {
        return make_error<IndexFile>(ErrorCode::IndexVersionMismatch, "Unsupported index version");
    }
    if (!declared_layout_fits(result.file.size(), *hdr)) {
        return make_error<IndexFile>(ErrorCode::IndexDamaged, "Index sections do not fit the file");
    }
    // What makes the bytes worth reading at all: that they are the ones a putup wrote, not a file
    // that merely starts with the right four and lays its sections out plausibly (#294).
    if (!index_verify_checksum(result)) {
        return make_error<IndexFile>(ErrorCode::IndexChecksumMismatch, "Build record failed its checksum");
    }

    return result;
}

} // namespace

auto open_index(std::string_view path) -> Result<IndexFile>
{
    return open_index_in_window(path, INDEX_VERSION);
}

auto read_index(IndexFile const& f) -> Result<Index>
{
    if (!index_is_open(f)) {
        return make_error<Index>(ErrorCode::InvalidState, "Index file not open");
    }

    auto index = Index {};

    // Read save_time_ns from header
    auto const* hdr = index_header(f);
    if (hdr) {
        index.set_save_time_ns(hdr->save_time_ns);
    }

    // A field this read cannot reproduce faithfully makes the whole record unreadable rather than
    // a weaker claim in its place: an empty name or operand set is something callers act on (#381).
    auto files = index_raw_files(f);
    for (auto i = std::size_t { 0 }; i < files.size(); ++i) {
        auto const& raw = files[i];
        auto name = index_get_semantic_string(f, raw.name_offset);
        if (!name) {
            return pup::unexpected<Error>(name.error());
        }
        auto entry = FileEntry::from_raw(raw, *name, i);
        if (!entry) {
            return pup::unexpected<Error>(entry.error());
        }
        index.add_file(*entry);
    }

    // Compute paths from parent chain (after all files loaded)
    index.compute_paths();

    // Read command entries (v8: instruction + operands)
    auto commands = index_raw_commands(f);
    for (auto i = std::size_t { 0 }; i < commands.size(); ++i) {
        auto const& raw = commands[i];
        auto instruction_pattern = index_get_string(f, raw.cmd_offset);
        auto display = index_get_string(f, raw.display_offset);
        auto env = index_get_semantic_string(f, raw.env_offset);
        if (!env) {
            return pup::unexpected<Error>(env.error());
        }
        auto operands = index_get_operands(f, i);
        if (!operands) {
            return pup::unexpected<Error>(operands.error());
        }
        auto command = CommandEntry::from_raw(
            raw, instruction_pattern, display, *env, std::move(operands->first), std::move(operands->second), i
        );
        if (!command) {
            return pup::unexpected<Error>(command.error());
        }
        index.add_command(std::move(*command));
    }

    // Read edges
    auto edges = index_raw_edges(f);
    for (auto const& raw : edges) {
        auto edge = EdgeEntry::from_raw(raw);
        if (!edge) {
            return pup::unexpected<Error>(edge.error());
        }
        index.add_edge(*edge);
    }

    // Build edge indices for O(1) lookup
    index.build_edge_indices();

    return index;
}

auto read_index(std::string_view path) -> Result<Index>
{
    auto file_result = open_index(path);
    if (!file_result) {
        return pup::unexpected<Error>(file_result.error());
    }
    return read_index(*file_result);
}

auto prior_paths(Index const& index) -> PriorPaths
{
    auto result = PriorPaths { .kind = PriorPaths::Kind::Known, .sources = {}, .generated = {}, .unowned = {} };

    for (auto const& file : index.files()) {
        if (pup::is_empty(file.path)) {
            continue;
        }
        // Total by construction: a reader that decides whether to overwrite a file must not
        // consume a projection that drops an entry type silently (#389).
        switch (file.type) {
        case NodeType::File:
            result.sources.push_back(file.path);
            break;
        case NodeType::Generated:
            result.generated.push_back(file.path);
            break;
        case NodeType::Ghost:
            result.unowned.push_back(file.path);
            break;
        case NodeType::Command:
        case NodeType::Directory:
        case NodeType::Variable:
        case NodeType::Group:
        case NodeType::GeneratedDir:
        case NodeType::Root:
        case NodeType::Condition:
        case NodeType::Phi:
            break;
        }
    }

    std::sort(result.sources.begin(), result.sources.end(), pup::handle_less);
    std::sort(result.generated.begin(), result.generated.end(), pup::handle_less);
    std::sort(result.unowned.begin(), result.unowned.end(), pup::handle_less);

    // Each entry feeds one list, so a path appearing twice -- in one list or across two -- is a
    // record holding two entries for one path, the state its own writer forbids, and this is the
    // last check available before an irreversible action (#382). Compared as stored: an
    // out-of-tree record legitimately holds one file as "a/bar.txt" and "build/a/bar.txt", which
    // stripping a prefix would collapse.
    auto const repeats = [](Vec<StringId> const& paths) {
        return std::adjacent_find(paths.begin(), paths.end()) != paths.end();
    };
    auto const overlaps = [](Vec<StringId> const& lhs, Vec<StringId> const& rhs) {
        auto l = lhs.begin();
        auto r = rhs.begin();
        while (l != lhs.end() && r != rhs.end()) {
            if (pup::handle_less(*l, *r)) {
                ++l;
            } else if (pup::handle_less(*r, *l)) {
                ++r;
            } else {
                return true;
            }
        }
        return false;
    };
    if (repeats(result.sources) || repeats(result.generated) || repeats(result.unowned)
        || overlaps(result.sources, result.generated) || overlaps(result.sources, result.unowned)
        || overlaps(result.generated, result.unowned)) {
        return PriorPaths { .kind = PriorPaths::Kind::Lost, .sources = {}, .generated = {}, .unowned = {} };
    }

    return result;
}

auto read_prior_paths(std::string_view path) -> PriorPaths
{
    if (!pup::platform::exists(path)) {
        return PriorPaths { .kind = PriorPaths::Kind::NeverBuilt, .sources = {}, .generated = {}, .unowned = {} };
    }

    auto lost = PriorPaths { .kind = PriorPaths::Kind::Lost, .sources = {}, .generated = {}, .unowned = {} };

    auto file = open_index_in_window(path, INDEX_LAYOUT_FLOOR);
    if (!file) {
        return lost;
    }

    // The file table and nothing else. Everything a version bump invalidates -- command
    // identities, signatures, recorded currency -- stays unread, so it cannot reach a caller
    // through this return type.
    auto recorded = Index {};
    auto raw = index_raw_files(*file);
    for (auto i = std::size_t { 0 }; i < raw.size(); ++i) {
        auto name = index_get_semantic_string(*file, raw[i].name_offset);
        if (!name) {
            return lost;
        }
        auto entry = FileEntry::from_raw(raw[i], *name, i);
        if (!entry) {
            return lost;
        }
        recorded.add_file(*entry);
    }
    recorded.compute_paths();

    return prior_paths(recorded);
}

auto index_header(IndexFile const& f) -> RawHeader const*
{
    if (!index_is_open(f) || f.file.size() < sizeof(RawHeader)) {
        return nullptr;
    }
    return reinterpret_cast<RawHeader const*>(f.file.data());
}

auto index_raw_files(IndexFile const& f) -> std::span<RawFileEntry const>
{
    auto const* hdr = index_header(f);
    return read_raw_entries<RawFileEntry>(f.file, hdr, hdr ? hdr->file_count : 0, hdr ? hdr->file_offset : 0);
}

auto index_raw_commands(IndexFile const& f) -> std::span<RawCommandEntry const>
{
    auto const* hdr = index_header(f);
    return read_raw_entries<RawCommandEntry>(f.file, hdr, hdr ? hdr->command_count : 0, hdr ? hdr->command_offset : 0);
}

auto index_raw_edges(IndexFile const& f) -> std::span<RawEdge const>
{
    auto const* hdr = index_header(f);
    return read_raw_entries<RawEdge>(f.file, hdr, hdr ? hdr->edge_count : 0, hdr ? hdr->edge_offset : 0);
}

namespace {

auto index_get_semantic_string(IndexFile const& f, std::uint32_t offset) -> Result<std::string_view>
{
    auto const* hdr = index_header(f);
    if (!hdr) {
        return make_error<std::string_view>(ErrorCode::IndexDamaged, "Index header unreadable");
    }

    // The string table's own end, not the file's: a string declared past the table but inside the
    // file is still a string this record does not contain.
    auto const table_end = std::size_t { hdr->string_offset } + hdr->string_table_size;
    auto const string_start = std::size_t { hdr->string_offset } + offset;

    // Length-prefixed strings: <u16 length><data>
    if (string_start + sizeof(std::uint16_t) > table_end) {
        return make_error<std::string_view>(ErrorCode::IndexDamaged, "Recorded string starts outside the string table");
    }

    auto data = std::span<std::byte const> { f.file.data(), f.file.size() };

    // Read u16 length (little-endian)
    auto const* len_bytes = data.subspan(string_start, sizeof(std::uint16_t)).data();
    auto const length = static_cast<std::uint16_t>(
        static_cast<std::uint8_t>(len_bytes[0]) | (static_cast<std::uint8_t>(len_bytes[1]) << 8)
    );

    // Offset 0 is the table's empty entry, so this is a value the record states, not a failure.
    if (length == 0) {
        return std::string_view {};
    }

    auto const data_start = string_start + sizeof(std::uint16_t);
    if (data_start + length > table_end) {
        return make_error<std::string_view>(ErrorCode::IndexDamaged, "Recorded string runs past the string table");
    }

    auto str_bytes = data.subspan(data_start, length);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return std::string_view { reinterpret_cast<char const*>(str_bytes.data()), length };
}

/// The display side of the same read: text a human is shown degrades to a marker that says so,
/// where a semantics-bearing field fails the record (#381). One bounds implementation, two
/// dispositions -- a second copy of the arithmetic is what #372 cost.
auto index_get_string(IndexFile const& f, std::uint32_t offset) -> std::string_view
{
    auto text = index_get_semantic_string(f, offset);
    return text ? *text : UNREADABLE_DISPLAY;
}

auto index_get_operands(IndexFile const& f, std::size_t cmd_index)
    -> Result<std::pair<Vec<NodeId>, Vec<NodeId>>>
{
    using Operands = std::pair<Vec<NodeId>, Vec<NodeId>>;

    auto const* hdr = index_header(f);
    if (!hdr || cmd_index >= hdr->command_count) {
        return make_error<Operands>(ErrorCode::IndexDamaged, "Operand record index outside the command table");
    }

    auto data = std::span<std::byte const> { f.file.data(), f.file.size() };

    // Read offset from operand table
    auto table_pos = std::size_t { hdr->operand_table_offset } + cmd_index * sizeof(std::uint32_t);
    if (table_pos + sizeof(std::uint32_t) > f.file.size()) {
        return make_error<Operands>(ErrorCode::IndexDamaged, "Operand table entry outside the file");
    }

    auto read_u32 = [&data](std::size_t at) {
        auto const* bytes = data.subspan(at, sizeof(std::uint32_t)).data();
        return static_cast<std::uint32_t>(
            static_cast<std::uint8_t>(bytes[0])
            | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[1])) << 8)
            | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[2])) << 16)
            | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[3])) << 24)
        );
    };

    auto offset = read_u32(table_pos);

    // Widened like every position here: in u32 this sum wraps, and the check below then passes (#372).
    // The operand section's own end, like the string table's: the writer lays the string table
    // directly after this section, so a record declared past it is one this record does not hold.
    auto const section_end = std::size_t { hdr->string_offset };
    auto record_pos = std::size_t { hdr->operand_data_offset } + offset;
    if (record_pos + 2 * sizeof(std::uint32_t) > section_end) {
        return make_error<Operands>(ErrorCode::IndexDamaged, "Operand record starts outside the operand section");
    }

    auto in_count = std::size_t { read_u32(record_pos) };
    auto out_count = std::size_t { read_u32(record_pos + sizeof(std::uint32_t)) };

    auto expected_size = 2 * sizeof(std::uint32_t) + (in_count + out_count) * sizeof(NodeId);
    if (record_pos + expected_size > section_end) {
        return make_error<Operands>(ErrorCode::IndexDamaged, "Operand record runs past the operand section");
    }

    auto inputs = Vec<NodeId> {};
    auto outputs = Vec<NodeId> {};
    inputs.reserve(in_count);
    outputs.reserve(out_count);

    auto pos = record_pos + 2 * sizeof(std::uint32_t);
    for (auto i = std::size_t { 0 }; i < in_count; ++i) {
        inputs.push_back(static_cast<NodeId>(read_u32(pos)));
        pos += sizeof(NodeId);
    }

    for (auto i = std::size_t { 0 }; i < out_count; ++i) {
        outputs.push_back(static_cast<NodeId>(read_u32(pos)));
        pos += sizeof(NodeId);
    }

    return Operands { std::move(inputs), std::move(outputs) };
}

} // namespace

auto index_verify_checksum(IndexFile const& f) -> bool
{
    if (!index_is_open(f) || f.file.size() < sizeof(RawFooter)) {
        return false;
    }

    auto const content_size = f.file.size() - sizeof(RawFooter);
    auto data = std::span<std::byte const> { f.file.data(), f.file.size() };

    auto computed = sha256(data.first(content_size));

    auto footer_bytes = data.subspan(content_size, sizeof(RawFooter));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto const* footer = reinterpret_cast<RawFooter const*>(footer_bytes.data());
    return computed == footer->checksum;
}

} // namespace pup::index
