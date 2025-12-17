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
    if (str.empty()) {
        return 0;
    }

    if (auto it = offsets_.find(std::string { str }); it != offsets_.end()) {
        return it->second;
    }

    auto constexpr MAX_OFFSET = std::numeric_limits<std::uint32_t>::max();
    if (data_.size() > MAX_OFFSET - str.size()) {
        throw std::overflow_error("String table exceeds 4GB limit");
    }

    auto offset = static_cast<std::uint32_t>(data_.size());
    data_.insert(data_.end(), str.begin(), str.end());
    offsets_.emplace(std::string { str }, offset);
    return offset;
}

auto IndexWriter::write(
    std::filesystem::path const& path,
    Index const& index) -> Result<void>
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

    // Calculate offsets
    auto const header_size = sizeof(RawHeader);
    auto const file_offset = header_size;
    auto const file_size = file_entries.size() * sizeof(RawFileEntry);
    auto const command_offset = file_offset + file_size;
    auto const command_size = command_entries.size() * sizeof(RawCommandEntry);
    auto const edge_offset = command_offset + command_size;
    auto const edge_size = edge_entries.size() * sizeof(RawEdge);
    auto const string_offset = edge_offset + edge_size;
    auto const string_size = strings.size();
    auto const footer_offset = string_offset + string_size;
    auto const total_size = footer_offset + sizeof(RawFooter);

    // Build header
    auto header = build_header(
        index, strings,
        file_offset, command_offset, edge_offset, string_offset);

    // Allocate result buffer
    auto result = std::vector<std::byte>(total_size);
    auto* ptr = result.data();

    // Write header
    std::memcpy(ptr, &header, sizeof(header));
    ptr += sizeof(header);

    // Write file entries
    if (!file_entries.empty()) {
        std::memcpy(ptr, file_entries.data(), file_size);
        ptr += file_size;
    }

    // Write command entries
    if (!command_entries.empty()) {
        std::memcpy(ptr, command_entries.data(), command_size);
        ptr += command_size;
    }

    // Write edge entries
    if (!edge_entries.empty()) {
        std::memcpy(ptr, edge_entries.data(), edge_size);
        ptr += edge_size;
    }

    // Write string table
    if (string_size > 0) {
        std::memcpy(ptr, strings.data().data(), string_size);
        ptr += string_size;
    }

    // Compute and write checksum
    auto content_span = std::span<std::byte const> { result.data(), footer_offset };
    auto checksum = sha256(content_span);
    auto footer = RawFooter { .checksum = checksum };
    std::memcpy(ptr, &footer, sizeof(footer));

    return result;
}

auto IndexWriter::build_header(
    Index const& index,
    StringTable const& strings,
    std::uint64_t file_offset,
    std::uint64_t command_offset,
    std::uint64_t edge_offset,
    std::uint64_t string_offset) -> RawHeader
{
    return RawHeader {
        .magic = INDEX_MAGIC,
        .version = INDEX_VERSION,
        .flags = 0,
        .file_count = static_cast<std::uint32_t>(index.files().size()),
        .command_count = static_cast<std::uint32_t>(index.commands().size()),
        .edge_count = static_cast<std::uint32_t>(index.edges().size()),
        .string_table_size = strings.size(),
        .reserved1 = 0,
        .file_offset = file_offset,
        .command_offset = command_offset,
        .edge_offset = edge_offset,
        .string_offset = string_offset,
    };
}

} // namespace pup::index
