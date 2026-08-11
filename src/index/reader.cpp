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

    if (result.file.size() < sizeof(RawHeader) + sizeof(RawFooter)) {
        return make_error<IndexFile>(ErrorCode::InvalidFormat, "Index file too small");
    }

    auto const* hdr = index_header(result);
    if (!hdr || std::memcmp(hdr->magic.data(), INDEX_MAGIC.data(), 4) != 0) {
        return make_error<IndexFile>(ErrorCode::InvalidFormat, "Invalid index file magic");
    }
    if (hdr->version < min_version || hdr->version > INDEX_VERSION) {
        return make_error<IndexFile>(ErrorCode::InvalidFormat, "Unsupported index version");
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

auto is_valid_index(std::string_view path) -> bool
{
    auto file = pup::platform::MappedFile::open(path);
    if (!file || file->size() < sizeof(RawHeader)) {
        return false;
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto const* header = reinterpret_cast<RawHeader const*>(file->data());
    return std::memcmp(header->magic.data(), INDEX_MAGIC.data(), 4) == 0
        && header->version == INDEX_VERSION;
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

    // Read file entries
    auto files = index_raw_files(f);
    for (auto i = std::size_t { 0 }; i < files.size(); ++i) {
        auto const& raw = files[i];
        auto name = index_get_string(f, raw.name_offset);
        index.add_file(FileEntry::from_raw(raw, name, i));
    }

    // Compute paths from parent chain (after all files loaded)
    index.compute_paths();

    // Read command entries (v8: instruction + operands)
    auto commands = index_raw_commands(f);
    for (auto i = std::size_t { 0 }; i < commands.size(); ++i) {
        auto const& raw = commands[i];
        auto instruction_pattern = index_get_string(f, raw.cmd_offset);
        auto display = index_get_string(f, raw.display_offset);
        auto env = index_get_string(f, raw.env_offset);
        auto [inputs, outputs] = index_get_operands(f, i);
        index.add_command(CommandEntry::from_raw(
            raw, instruction_pattern, display, env, std::move(inputs), std::move(outputs), i
        ));
    }

    // Read edges
    auto edges = index_raw_edges(f);
    for (auto const& raw : edges) {
        index.add_edge(EdgeEntry::from_raw(raw));
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
    auto result = PriorPaths { .kind = PriorPaths::Kind::Known, .sources = {}, .generated = {} };

    for (auto const& file : index.files()) {
        if (pup::is_empty(file.path)) {
            continue;
        }
        if (file.type == NodeType::File) {
            result.sources.push_back(file.path);
        } else if (file.type == NodeType::Generated) {
            result.generated.push_back(file.path);
        }
    }

    std::sort(result.sources.begin(), result.sources.end(), pup::handle_less);
    std::sort(result.generated.begin(), result.generated.end(), pup::handle_less);
    return result;
}

auto read_prior_paths(std::string_view path) -> PriorPaths
{
    if (!pup::platform::exists(path)) {
        return PriorPaths { .kind = PriorPaths::Kind::NeverBuilt, .sources = {}, .generated = {} };
    }

    auto lost = PriorPaths { .kind = PriorPaths::Kind::Lost, .sources = {}, .generated = {} };

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
        recorded.add_file(FileEntry::from_raw(raw[i], index_get_string(*file, raw[i].name_offset), i));
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

auto index_get_string(IndexFile const& f, std::uint32_t offset) -> std::string_view
{
    auto const* hdr = index_header(f);
    if (!hdr) {
        return {};
    }

    // Length-prefixed strings: <u16 length><data>
    auto const string_start = std::size_t { hdr->string_offset } + offset;

    // Need at least 2 bytes for length prefix
    if (string_start + sizeof(std::uint16_t) > f.file.size()) {
        return {};
    }

    auto data = std::span<std::byte const> { f.file.data(), f.file.size() };

    // Read u16 length (little-endian)
    auto const* len_bytes = data.subspan(string_start, sizeof(std::uint16_t)).data();
    auto const length = static_cast<std::uint16_t>(
        static_cast<std::uint8_t>(len_bytes[0]) | (static_cast<std::uint8_t>(len_bytes[1]) << 8)
    );

    if (length == 0) {
        return {};
    }

    // Bounds check string data
    auto const data_start = string_start + sizeof(std::uint16_t);
    if (data_start + length > f.file.size()) {
        return {};
    }

    auto str_bytes = data.subspan(data_start, length);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return { reinterpret_cast<char const*>(str_bytes.data()), length };
}

auto index_get_operands(IndexFile const& f, std::size_t cmd_index)
    -> std::pair<Vec<NodeId>, Vec<NodeId>>
{
    auto const* hdr = index_header(f);
    if (!hdr || cmd_index >= hdr->command_count) {
        return { {}, {} };
    }

    auto data = std::span<std::byte const> { f.file.data(), f.file.size() };

    // Read offset from operand table
    auto table_pos = std::size_t { hdr->operand_table_offset } + cmd_index * sizeof(std::uint32_t);
    if (table_pos + sizeof(std::uint32_t) > f.file.size()) {
        return { {}, {} };
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
    auto record_pos = std::size_t { hdr->operand_data_offset } + offset;
    if (record_pos + 2 * sizeof(std::uint32_t) > f.file.size()) {
        return { {}, {} };
    }

    auto in_count = std::size_t { read_u32(record_pos) };
    auto out_count = std::size_t { read_u32(record_pos + sizeof(std::uint32_t)) };

    auto expected_size = 2 * sizeof(std::uint32_t) + (in_count + out_count) * sizeof(NodeId);
    if (record_pos + expected_size > f.file.size()) {
        return { {}, {} };
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

    return { std::move(inputs), std::move(outputs) };
}

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
