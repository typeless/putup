// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/index/reader.hpp"
#include "pup/core/hash.hpp"

#include <cstring>
#include <fcntl.h>
#include <span>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pup::index {

IndexReader::~IndexReader()
{
    close();
}

IndexReader::IndexReader(IndexReader&& other) noexcept
    : data_(other.data_)
    , size_(other.size_)
    , fd_(other.fd_)
{
    other.data_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
}

auto IndexReader::operator=(IndexReader&& other) noexcept -> IndexReader&
{
    if (this != &other) {
        close();
        data_ = other.data_;
        size_ = other.size_;
        fd_ = other.fd_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.fd_ = -1;
    }
    return *this;
}

auto IndexReader::open(std::filesystem::path const& path) -> Result<IndexReader>
{
    auto reader = IndexReader {};

    reader.fd_ = ::open(path.c_str(), O_RDONLY);
    if (reader.fd_ < 0)
        return make_error<IndexReader>(ErrorCode::IoError, "Failed to open index file");

    struct stat st;
    if (::fstat(reader.fd_, &st) < 0) {
        ::close(reader.fd_);
        return make_error<IndexReader>(ErrorCode::IoError, "Failed to stat index file");
    }

    reader.size_ = static_cast<std::size_t>(st.st_size);

    if (reader.size_ < sizeof(RawHeader) + sizeof(RawFooter)) {
        ::close(reader.fd_);
        return make_error<IndexReader>(ErrorCode::InvalidFormat, "Index file too small");
    }

    reader.data_ = ::mmap(nullptr, reader.size_, PROT_READ, MAP_PRIVATE, reader.fd_, 0);
    if (reader.data_ == MAP_FAILED) {
        reader.data_ = nullptr;
        ::close(reader.fd_);
        return make_error<IndexReader>(ErrorCode::IoError, "Failed to mmap index file");
    }

    // Validate header
    auto const* hdr = reader.header();
    if (!hdr || std::memcmp(hdr->magic.data(), INDEX_MAGIC.data(), 4) != 0) {
        reader.close();
        return make_error<IndexReader>(ErrorCode::InvalidFormat, "Invalid index file magic");
    }
    // Support reading older versions (1 and 2)
    if (hdr->version < 1 || hdr->version > INDEX_VERSION) {
        reader.close();
        return make_error<IndexReader>(ErrorCode::InvalidFormat, "Unsupported index version");
    }

    return reader;
}

auto IndexReader::is_valid_index(std::filesystem::path const& path) -> bool
{
    auto fd = int { ::open(path.c_str(), O_RDONLY) };
    if (fd < 0)
        return false;

    auto header = RawHeader {};
    auto n = ssize_t { ::read(fd, &header, sizeof(header)) };
    ::close(fd);

    if (n != static_cast<ssize_t>(sizeof(header)))
        return false;

    // Accept versions 1 and 2
    return std::memcmp(header.magic.data(), INDEX_MAGIC.data(), 4) == 0 && header.version >= 1 && header.version <= INDEX_VERSION;
}

auto IndexReader::read() const -> Result<Index>
{
    if (!is_open())
        return make_error<Index>(ErrorCode::InvalidState, "Reader not open");

    auto index = Index {};

    // Read file entries
    auto files = raw_files();
    for (auto const& raw : files) {
        auto name = get_string(raw.name_offset, raw.name_length);
        index.add_file(FileEntry::from_raw(raw, name));
    }

    // Compute paths from parent chain (after all files loaded)
    index.compute_paths();

    // Read command entries
    auto commands = raw_commands();
    for (auto const& raw : commands) {
        auto cmd = get_string(raw.cmd_offset, raw.cmd_length);
        auto display = get_string(raw.display_offset, raw.display_length);
        auto env = get_string(raw.env_offset, raw.env_length);
        index.add_command(CommandEntry::from_raw(raw, cmd, display, env));
    }

    // Read edges
    auto edges = raw_edges();
    for (auto const& raw : edges)
        index.add_edge(EdgeEntry::from_raw(raw));

    // Build edge indices for O(1) lookup
    index.build_edge_indices();

    return index;
}

auto IndexReader::header() const -> RawHeader const*
{
    if (!is_open() || size_ < sizeof(RawHeader))
        return nullptr;
    return static_cast<RawHeader const*>(data_);
}

auto IndexReader::raw_files() const -> std::span<RawFileEntry const>
{
    auto const* hdr = header();
    if (!hdr || hdr->file_count == 0)
        return {};

    auto const* base = static_cast<std::byte const*>(data_);
    auto const* files = reinterpret_cast<RawFileEntry const*>(base + hdr->file_offset);
    return { files, hdr->file_count };
}

auto IndexReader::raw_commands() const -> std::span<RawCommandEntry const>
{
    auto const* hdr = header();
    if (!hdr || hdr->command_count == 0)
        return {};

    auto const* base = static_cast<std::byte const*>(data_);
    auto const* commands = reinterpret_cast<RawCommandEntry const*>(base + hdr->command_offset);
    return { commands, hdr->command_count };
}

auto IndexReader::raw_edges() const -> std::span<RawEdge const>
{
    auto const* hdr = header();
    if (!hdr || hdr->edge_count == 0)
        return {};

    auto const* base = static_cast<std::byte const*>(data_);
    auto const* edges = reinterpret_cast<RawEdge const*>(base + hdr->edge_offset);
    return { edges, hdr->edge_count };
}

auto IndexReader::get_string(std::uint32_t offset, std::uint32_t length) const -> std::string_view
{
    auto const* hdr = header();
    if (!hdr || length == 0)
        return {};

    auto const* base = static_cast<char const*>(data_);
    auto const string_start = hdr->string_offset + offset;

    if (string_start + length > size_)
        return {};

    return { base + string_start, length };
}

auto IndexReader::verify_checksum() const -> bool
{
    if (!is_open() || size_ < sizeof(RawFooter))
        return false;

    auto const content_size = size_ - sizeof(RawFooter);
    auto const* base = static_cast<std::byte const*>(data_);

    auto content_span = std::span<std::byte const> { base, content_size };
    auto computed = sha256(content_span);

    auto const* footer = reinterpret_cast<RawFooter const*>(base + content_size);
    return computed == footer->checksum;
}

auto IndexReader::close() -> void
{
    if (data_) {
        ::munmap(data_, size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    size_ = 0;
}

} // namespace pup::index
