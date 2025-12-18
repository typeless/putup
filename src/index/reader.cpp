// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/index/reader.hpp"
#include "pup/core/hash.hpp"

#include <cstring>
#include <fstream>
#include <span>

namespace pup::index {

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
    if (!hdr || hdr->file_count == 0) {
        return {};
    }

    // Bounds check: ensure offset and size are within file
    auto const size = std::size_t { hdr->file_count } * sizeof(RawFileEntry);
    if (hdr->file_offset > file_.size() || size > file_.size() - hdr->file_offset) {
        return {};
    }

    auto data = std::span<std::byte const> { file_.data(), file_.size() };
    auto file_bytes = data.subspan(hdr->file_offset, size);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto const* files = reinterpret_cast<RawFileEntry const*>(file_bytes.data());
    return { files, hdr->file_count };
}

auto IndexReader::raw_commands() const -> std::span<RawCommandEntry const>
{
    auto const* hdr = header();
    if (!hdr || hdr->command_count == 0) {
        return {};
    }

    // Bounds check: ensure offset and size are within file
    auto const size = std::size_t { hdr->command_count } * sizeof(RawCommandEntry);
    if (hdr->command_offset > file_.size() || size > file_.size() - hdr->command_offset) {
        return {};
    }

    auto data = std::span<std::byte const> { file_.data(), file_.size() };
    auto cmd_bytes = data.subspan(hdr->command_offset, size);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto const* commands = reinterpret_cast<RawCommandEntry const*>(cmd_bytes.data());
    return { commands, hdr->command_count };
}

auto IndexReader::raw_edges() const -> std::span<RawEdge const>
{
    auto const* hdr = header();
    if (!hdr || hdr->edge_count == 0) {
        return {};
    }

    // Bounds check: ensure offset and size are within file
    auto const size = std::size_t { hdr->edge_count } * sizeof(RawEdge);
    if (hdr->edge_offset > file_.size() || size > file_.size() - hdr->edge_offset) {
        return {};
    }

    auto data = std::span<std::byte const> { file_.data(), file_.size() };
    auto edge_bytes = data.subspan(hdr->edge_offset, size);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto const* edges = reinterpret_cast<RawEdge const*>(edge_bytes.data());
    return { edges, hdr->edge_count };
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
