// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/index/writer.hpp"
#include "pup/core/hash.hpp"
#include "pup/platform/file_io.hpp"

#include <cstring>
#include <span>

namespace pup::index {

auto IndexWriter::StringTable::add(std::string_view str) -> std::uint32_t
{
    // Empty strings get offset 0, which has a zero-length entry
    if (str.empty()) {
        // Ensure offset 0 has a zero-length prefix if this is the first call
        if (data_.empty()) {
            data_.push_back(0);
            data_.push_back(0);
        }
        return 0;
    }

    // Check for duplicate
    if (auto it = offsets_.find(std::string { str }); it != offsets_.end()) {
        return it->second;
    }

    // Validate string length fits in u16
    auto constexpr MAX_STRING_LENGTH = std::uint16_t { 0xFFFF };
    if (str.size() > MAX_STRING_LENGTH) {
        throw std::overflow_error("String exceeds 64KB limit");
    }

    // Ensure we have the empty string entry at offset 0 (handles case where
    // first add() call is a non-empty string - the empty case is handled above)
    if (data_.empty()) {
        data_.push_back(0);
        data_.push_back(0);
    }

    // Check table size limit (u32 offset max)
    auto constexpr MAX_OFFSET = std::numeric_limits<std::uint32_t>::max();
    auto const entry_size = sizeof(std::uint16_t) + str.size();
    if (data_.size() > MAX_OFFSET - entry_size) {
        throw std::overflow_error("String table exceeds 4GB limit");
    }

    auto offset = static_cast<std::uint32_t>(data_.size());

    // Write length prefix (u16, little-endian)
    auto const length = static_cast<std::uint16_t>(str.size());
    data_.push_back(static_cast<char>(length & 0xFF));
    data_.push_back(static_cast<char>(length >> 8));

    // Write string data
    data_.insert(data_.end(), str.begin(), str.end());

    offsets_.emplace(std::string { str }, offset);
    return offset;
}

auto IndexWriter::write(
    std::filesystem::path const& path,
    Index const& index
) -> Result<void>
{
    auto data = Result<std::vector<std::byte>> { serialize(index) };
    if (!data) {
        return pup::unexpected<Error>(data.error());
    }

    return pup::platform::atomic_write(path, *data);
}

auto IndexWriter::serialize(Index const& index) -> Result<std::vector<std::byte>>
{
    auto strings = StringTable {};

    // Build file entries and collect strings
    auto file_entries = std::vector<RawFileEntry> {};
    file_entries.reserve(index.files().size());

    for (auto const& file : index.files()) {
        auto name_offset = strings.add(file.name);
        file_entries.push_back(file.to_raw(name_offset));
    }

    // Build command entries and collect strings
    auto command_entries = std::vector<RawCommandEntry> {};
    command_entries.reserve(index.commands().size());

    for (auto const& cmd : index.commands()) {
        auto cmd_offset = strings.add(cmd.command);
        auto display_offset = strings.add(cmd.display);
        auto env_offset = strings.add(cmd.env);
        command_entries.push_back(cmd.to_raw(cmd_offset, display_offset, env_offset));
    }

    // Build edge entries
    auto edge_entries = std::vector<RawEdge> {};
    edge_entries.reserve(index.edges().size());

    for (auto const& edge : index.edges()) {
        edge_entries.push_back(edge.to_raw());
    }

    // Calculate offsets (all u32, check for overflow)
    auto constexpr MAX_U32 = std::numeric_limits<std::uint32_t>::max();
    auto const file_size_64 = file_entries.size() * sizeof(RawFileEntry);
    auto const command_size_64 = command_entries.size() * sizeof(RawCommandEntry);
    auto const edge_size_64 = edge_entries.size() * sizeof(RawEdge);
    auto const total_size_64 = sizeof(RawHeader) + file_size_64 + command_size_64
        + edge_size_64 + strings.size() + sizeof(RawFooter);

    if (total_size_64 > MAX_U32) {
        return make_error<std::vector<std::byte>>(
            ErrorCode::InvalidState, "Index exceeds 4GB limit"
        );
    }

    auto const header_size = static_cast<std::uint32_t>(sizeof(RawHeader));
    auto const file_offset = header_size;
    auto const file_size = static_cast<std::uint32_t>(file_size_64);
    auto const command_offset = file_offset + file_size;
    auto const command_size = static_cast<std::uint32_t>(command_size_64);
    auto const edge_offset = command_offset + command_size;
    auto const edge_size = static_cast<std::uint32_t>(edge_size_64);
    auto const string_offset = edge_offset + edge_size;
    auto const string_size = strings.size();
    auto const footer_offset = string_offset + string_size;
    auto const total_size = static_cast<std::uint32_t>(total_size_64);

    // Build header
    auto header = build_header(
        index, strings, file_offset, command_offset, edge_offset, string_offset
    );

    // Allocate result buffer
    auto result = std::vector<std::byte>(total_size);
    auto output = std::span<std::byte> { result };

    // Write header
    std::memcpy(output.subspan(0, sizeof(header)).data(), &header, sizeof(header));

    // Write file entries
    if (!file_entries.empty()) {
        std::memcpy(output.subspan(file_offset, file_size).data(), file_entries.data(), file_size);
    }

    // Write command entries
    if (!command_entries.empty()) {
        std::memcpy(output.subspan(command_offset, command_size).data(), command_entries.data(), command_size);
    }

    // Write edge entries
    if (!edge_entries.empty()) {
        std::memcpy(output.subspan(edge_offset, edge_size).data(), edge_entries.data(), edge_size);
    }

    // Write string table
    if (string_size > 0) {
        std::memcpy(output.subspan(string_offset, string_size).data(), strings.data().data(), string_size);
    }

    // Compute and write checksum
    auto checksum = sha256(output.first(footer_offset));
    auto footer = RawFooter { .checksum = checksum };
    std::memcpy(output.subspan(footer_offset, sizeof(footer)).data(), &footer, sizeof(footer));

    return result;
}

auto IndexWriter::build_header(
    Index const& index,
    StringTable const& strings,
    std::uint32_t file_offset,
    std::uint32_t command_offset,
    std::uint32_t edge_offset,
    std::uint32_t string_offset
) -> RawHeader
{
    return RawHeader {
        .magic = INDEX_MAGIC,
        .version = INDEX_VERSION,
        .file_count = static_cast<std::uint32_t>(index.files().size()),
        .command_count = static_cast<std::uint32_t>(index.commands().size()),
        .edge_count = static_cast<std::uint32_t>(index.edges().size()),
        .string_table_size = strings.size(),
        .file_offset = file_offset,
        .command_offset = command_offset,
        .edge_offset = edge_offset,
        .string_offset = string_offset,
    };
}

} // namespace pup::index
