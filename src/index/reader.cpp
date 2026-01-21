// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/index/reader.hpp"
#include "pup/core/hash.hpp"

#include <cstring>
#include <fstream>
#include <span>

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

} // namespace

auto IndexReader::open(std::filesystem::path const& path) -> Result<IndexReader>
{
    auto reader = IndexReader {};

    auto file_result = pup::platform::MappedFile::open(path);
    if (!file_result) {
        return make_error<IndexReader>(ErrorCode::IoError, "Failed to open index file");
    }

    reader.file_ = std::move(*file_result);

    if (reader.file_.size() < sizeof(RawHeader) + sizeof(RawFooter)) {
        return make_error<IndexReader>(ErrorCode::InvalidFormat, "Index file too small");
    }

    // Validate header
    auto const* hdr = reader.header();
    if (!hdr || std::memcmp(hdr->magic.data(), INDEX_MAGIC.data(), 4) != 0) {
        return make_error<IndexReader>(ErrorCode::InvalidFormat, "Invalid index file magic");
    }
    // v6 format only (clean break from v5)
    if (hdr->version != INDEX_VERSION) {
        return make_error<IndexReader>(ErrorCode::InvalidFormat, "Unsupported index version");
    }

    return reader;
}

auto IndexReader::is_valid_index(std::filesystem::path const& path) -> bool
{
    auto file = std::ifstream { path, std::ios::binary };
    if (!file) {
        return false;
    }

    auto header = RawHeader {};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (!file || file.gcount() != static_cast<std::streamsize>(sizeof(header))) {
        return false;
    }

    // v6 format only
    return std::memcmp(header.magic.data(), INDEX_MAGIC.data(), 4) == 0
        && header.version == INDEX_VERSION;
}

auto IndexReader::read() const -> Result<Index>
{
    if (!is_open()) {
        return make_error<Index>(ErrorCode::InvalidState, "Reader not open");
    }

    auto index = Index {};

    // Read file entries
    auto files = raw_files();
    for (auto i = std::size_t { 0 }; i < files.size(); ++i) {
        auto const& raw = files[i];
        auto name = get_string(raw.name_offset);
        index.add_file(FileEntry::from_raw(raw, name, i));
    }

    // Compute paths from parent chain (after all files loaded)
    index.compute_paths();

    // Read command entries
    auto commands = raw_commands();
    for (auto i = std::size_t { 0 }; i < commands.size(); ++i) {
        auto const& raw = commands[i];
        auto cmd = get_string(raw.cmd_offset);
        auto display = get_string(raw.display_offset);
        auto env = get_string(raw.env_offset);
        index.add_command(CommandEntry::from_raw(raw, cmd, display, env, i));
    }

    // Read edges
    auto edges = raw_edges();
    for (auto const& raw : edges) {
        index.add_edge(EdgeEntry::from_raw(raw));
    }

    // Build edge indices for O(1) lookup
    index.build_edge_indices();

    return index;
}

auto IndexReader::header() const -> RawHeader const*
{
    if (!is_open() || file_.size() < sizeof(RawHeader)) {
        return nullptr;
    }
    return reinterpret_cast<RawHeader const*>(file_.data());
}

auto IndexReader::raw_files() const -> std::span<RawFileEntry const>
{
    auto const* hdr = header();
    return read_raw_entries<RawFileEntry>(file_, hdr, hdr ? hdr->file_count : 0, hdr ? hdr->file_offset : 0);
}

auto IndexReader::raw_commands() const -> std::span<RawCommandEntry const>
{
    auto const* hdr = header();
    return read_raw_entries<RawCommandEntry>(file_, hdr, hdr ? hdr->command_count : 0, hdr ? hdr->command_offset : 0);
}

auto IndexReader::raw_edges() const -> std::span<RawEdge const>
{
    auto const* hdr = header();
    return read_raw_entries<RawEdge>(file_, hdr, hdr ? hdr->edge_count : 0, hdr ? hdr->edge_offset : 0);
}

auto IndexReader::get_string(std::uint32_t offset) const -> std::string_view
{
    auto const* hdr = header();
    if (!hdr) {
        return {};
    }

    // Length-prefixed strings: <u16 length><data>
    auto const string_start = hdr->string_offset + offset;

    // Need at least 2 bytes for length prefix
    if (string_start + sizeof(std::uint16_t) > file_.size()) {
        return {};
    }

    auto data = std::span<std::byte const> { file_.data(), file_.size() };

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
    if (data_start + length > file_.size()) {
        return {};
    }

    auto str_bytes = data.subspan(data_start, length);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return { reinterpret_cast<char const*>(str_bytes.data()), length };
}

auto IndexReader::verify_checksum() const -> bool
{
    if (!is_open() || file_.size() < sizeof(RawFooter)) {
        return false;
    }

    auto const content_size = file_.size() - sizeof(RawFooter);
    auto data = std::span<std::byte const> { file_.data(), file_.size() };

    auto computed = sha256(data.first(content_size));

    auto footer_bytes = data.subspan(content_size, sizeof(RawFooter));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto const* footer = reinterpret_cast<RawFooter const*>(footer_bytes.data());
    return computed == footer->checksum;
}

} // namespace pup::index
