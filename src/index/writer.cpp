// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/index/writer.hpp"
#include "pup/core/hash.hpp"

#include <cstring>
#include <fcntl.h>
#include <random>
#include <span>
#include <unistd.h>

namespace pup::index {

auto IndexWriter::StringTable::add(std::string_view str) -> std::uint32_t
{
    if (str.empty())
        return 0;

    auto offset = static_cast<std::uint32_t>(data_.size());
    data_.insert(data_.end(), str.begin(), str.end());
    return offset;
}

auto IndexWriter::write(
    std::filesystem::path const& path,
    Index const& index) -> Result<void>
{
    auto data = serialize(index);
    if (!data)
        return pup::unexpected<Error>(data.error());

    // Create parent directories if needed
    auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec)
            return make_error<void>(ErrorCode::IoError, "Failed to create directory");
    }

    // Generate temporary filename
    auto temp_path = path;
    temp_path += ".tmp.";

    auto rd = std::random_device {};
    auto gen = std::mt19937 { rd() };
    auto dist = std::uniform_int_distribution<> { 0, 15 };
    auto const hex = "0123456789abcdef";
    for (auto i = 0; i < 8; ++i)
        temp_path += hex[dist(gen)];

    // Write to temporary file
    auto fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return make_error<void>(ErrorCode::IoError, "Failed to create temporary file");

    auto bytes_written = ::write(fd, data->data(), data->size());
    auto write_error = (bytes_written != static_cast<ssize_t>(data->size()));

    if (::fsync(fd) < 0)
        write_error = true;

    ::close(fd);

    if (write_error) {
        ::unlink(temp_path.c_str());
        return make_error<void>(ErrorCode::IoError, "Failed to write index file");
    }

    // Atomic rename
    if (::rename(temp_path.c_str(), path.c_str()) < 0) {
        ::unlink(temp_path.c_str());
        return make_error<void>(ErrorCode::IoError, "Failed to rename index file");
    }

    return {};
}

auto IndexWriter::serialize(Index const& index) -> Result<std::vector<std::byte>>
{
    auto strings = StringTable {};

    // Build file entries and collect strings
    auto file_entries = std::vector<RawFileEntry> {};
    file_entries.reserve(index.files().size());

    for (auto const& file : index.files()) {
        auto path_offset = strings.add(file.path);
        file_entries.push_back(file.to_raw(path_offset));
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

    for (auto const& edge : index.edges())
        edge_entries.push_back(edge.to_raw());

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
