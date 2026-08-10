// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/index/writer.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/clock.hpp"
#include "pup/core/expected.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/hash.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"
#include "pup/index/entry.hpp"
#include "pup/index/format.hpp"
#include "pup/platform/file_io.hpp"

#include "pup/core/sorted_id_vec.hpp"
#include "pup/core/string_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <tuple>

namespace pup::index {

namespace {

constexpr auto MAX_U32 = std::numeric_limits<std::uint32_t>::max();

/// A string table entry is length-prefixed with a u16, so this is a format constant.
constexpr auto MAX_STRING_LENGTH = std::size_t { 0xFFFF };

/// String table builder (internal helper)
///
/// Deduplicates strings using StringPool for identity and SortedPairVec
/// to map interned StringId → byte offset in the serialized table.
class StringTable final {
public:
    [[nodiscard]]
    auto add(std::string_view str) -> Result<std::uint32_t>;

    [[nodiscard]]
    auto data() const -> Vec<char> const&
    {
        return data_;
    }

    [[nodiscard]]
    auto size() const -> std::uint32_t
    {
        return static_cast<std::uint32_t>(data_.size());
    }

private:
    Vec<char> data_ = {};
    StringPool pool_ = {};
    SortedPairVec offsets_ = {};
};

auto StringTable::add(std::string_view str) -> Result<std::uint32_t>
{
    // Empty strings get offset 0, which has a zero-length entry
    if (str.empty()) {
        if (data_.empty()) {
            data_.push_back(0);
            data_.push_back(0);
        }
        return 0;
    }

    // Check for duplicate via StringPool
    auto str_id = pool_.intern(str);
    auto const* existing = offsets_.find(to_underlying(str_id));
    if (existing) {
        return *existing;
    }

    // Validate string length fits in u16
    if (str.size() > MAX_STRING_LENGTH) {
        return make_error<std::uint32_t>(
            ErrorCode::InvalidArgument, "String exceeds 64KB limit"
        );
    }

    // Ensure we have the empty string entry at offset 0
    if (data_.empty()) {
        data_.push_back(0);
        data_.push_back(0);
    }

    // Check table size limit (u32 offset max)
    auto const entry_size = sizeof(std::uint16_t) + str.size();
    if (data_.size() > MAX_U32 - entry_size) {
        return make_error<std::uint32_t>(
            ErrorCode::InvalidArgument, "String table exceeds 4GB limit"
        );
    }

    auto offset = static_cast<std::uint32_t>(data_.size());

    // Write length prefix (u16, little-endian)
    auto const length = static_cast<std::uint16_t>(str.size());
    data_.push_back(static_cast<char>(length & 0xFF));
    data_.push_back(static_cast<char>(length >> 8));

    // Write string data
    data_.insert(data_.end(), str.begin(), str.end());

    offsets_.insert(to_underlying(str_id), offset);
    return offset;
}

auto build_header(
    Index const& index,
    StringTable const& strings,
    std::uint32_t file_offset,
    std::uint32_t command_offset,
    std::uint32_t edge_offset,
    std::uint32_t operand_table_offset,
    std::uint32_t operand_data_offset,
    std::uint32_t string_offset,
    std::int64_t save_time_ns
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
        .operand_table_offset = operand_table_offset,
        .operand_data_offset = operand_data_offset,
        .string_offset = string_offset,
        .save_time_ns = save_time_ns,
    };
}

} // namespace

auto write_index(
    std::string_view path,
    Index const& index
) -> Result<void>
{
    auto data = serialize_index(index);
    if (!data) {
        return pup::unexpected<Error>(data.error());
    }

    return pup::platform::atomic_write(path, *data);
}

auto serialize_index(Index const& index) -> Result<Vec<std::byte>>
{
    // Load derives ids from position, so a gap silently rewires every later edge.
    for (auto i = std::size_t { 0 }; i < index.files().size(); ++i) {
        if (index.files()[i].id != static_cast<NodeId>(i + 1)) {
            return make_error<Vec<std::byte>>(
                ErrorCode::IndexCorrupted,
                "file ids must be dense: id == position + 1"
            );
        }
    }
    for (auto i = std::size_t { 0 }; i < index.commands().size(); ++i) {
        if (index.commands()[i].id != node_id::make_command(i + 1)) {
            return make_error<Vec<std::byte>>(
                ErrorCode::IndexCorrupted,
                "command ids must be dense: id == make_command(position + 1)"
            );
        }
    }

    auto strings = StringTable {};

    // Identity is key/signature, not this text (#360): mark and shorten rather than fail the record.
    auto add_display = [&strings](std::string_view text) -> Result<std::uint32_t> {
        if (text.size() <= MAX_STRING_LENGTH) {
            return strings.add(text);
        }
        auto marker = Buf {};
        marker.fmt("...[truncated, {} bytes total]", text.size());
        auto kept = MAX_STRING_LENGTH - std::min(marker.view().size(), MAX_STRING_LENGTH);
        auto projected = Buf {};
        projected += text.substr(0, kept);
        projected += marker.view();
        return strings.add(projected.view().substr(0, MAX_STRING_LENGTH));
    };

    // Build file entries and collect strings
    auto file_entries = Vec<RawFileEntry> {};
    file_entries.reserve(index.files().size());

    auto& pool = global_pool();
    for (auto const& file : index.files()) {
        auto name_offset = strings.add(pool.get(file.name));
        if (!name_offset) {
            return pup::unexpected<Error>(name_offset.error());
        }
        file_entries.push_back(file.to_raw(*name_offset));
    }

    // Build command entries and collect strings
    // v8: Use instruction_pattern instead of fully-expanded command
    auto command_entries = Vec<RawCommandEntry> {};
    command_entries.reserve(index.commands().size());

    for (auto const& cmd : index.commands()) {
        auto instruction_offset = add_display(pool.get(cmd.instruction_pattern));
        if (!instruction_offset) {
            return pup::unexpected<Error>(instruction_offset.error());
        }
        auto display_offset = add_display(pool.get(cmd.display));
        if (!display_offset) {
            return pup::unexpected<Error>(display_offset.error());
        }
        auto env_offset = strings.add(pool.get(cmd.env));
        if (!env_offset) {
            return pup::unexpected<Error>(env_offset.error());
        }
        command_entries.push_back(cmd.to_raw(*instruction_offset, *display_offset, *env_offset));
    }

    // v8: Build operand offset table and operand data
    auto operand_table = Vec<std::uint32_t> {};
    auto operand_data = Vec<std::byte> {};
    operand_table.reserve(index.commands().size());

    for (auto const& cmd : index.commands()) {
        operand_table.push_back(static_cast<std::uint32_t>(operand_data.size()));

        // Write input count (1 byte, max 255 inputs)
        auto input_count = std::min(cmd.inputs.size(), std::size_t { 255 });
        operand_data.push_back(static_cast<std::byte>(input_count));

        // Write output count (1 byte, max 255 outputs)
        auto output_count = std::min(cmd.outputs.size(), std::size_t { 255 });
        operand_data.push_back(static_cast<std::byte>(output_count));

        // Write input NodeIds (4 bytes each)
        for (std::size_t i = 0; i < input_count; ++i) {
            auto id = cmd.inputs[i];
            operand_data.push_back(static_cast<std::byte>(id & 0xFF));
            operand_data.push_back(static_cast<std::byte>((id >> 8) & 0xFF));
            operand_data.push_back(static_cast<std::byte>((id >> 16) & 0xFF));
            operand_data.push_back(static_cast<std::byte>((id >> 24) & 0xFF));
        }

        // Write output NodeIds (4 bytes each)
        for (std::size_t i = 0; i < output_count; ++i) {
            auto id = cmd.outputs[i];
            operand_data.push_back(static_cast<std::byte>(id & 0xFF));
            operand_data.push_back(static_cast<std::byte>((id >> 8) & 0xFF));
            operand_data.push_back(static_cast<std::byte>((id >> 16) & 0xFF));
            operand_data.push_back(static_cast<std::byte>((id >> 24) & 0xFF));
        }
    }

    // Build edge entries
    auto edge_entries = Vec<RawEdge> {};
    edge_entries.reserve(index.edges().size());

    for (auto const& edge : index.edges()) {
        edge_entries.push_back(edge.to_raw());
    }

    // Canonical order: edge bytes must not depend on job-completion order (#120).
    std::sort(edge_entries.begin(), edge_entries.end(), [](RawEdge const& a, RawEdge const& b) {
        return std::tie(a.from_id, a.to_id, a.type) < std::tie(b.from_id, b.to_id, b.type);
    });

    // Calculate offsets (all u32, check for overflow)
    auto const file_size_64 = file_entries.size() * sizeof(RawFileEntry);
    auto const command_size_64 = command_entries.size() * sizeof(RawCommandEntry);
    auto const edge_size_64 = edge_entries.size() * sizeof(RawEdge);
    auto const operand_table_size_64 = operand_table.size() * sizeof(std::uint32_t);
    auto const operand_data_size_64 = operand_data.size();
    auto const total_size_64 = sizeof(RawHeader) + file_size_64 + command_size_64
        + edge_size_64 + operand_table_size_64 + operand_data_size_64
        + strings.size() + sizeof(RawFooter);

    if (total_size_64 > MAX_U32) {
        return make_error<Vec<std::byte>>(
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
    auto const operand_table_offset = edge_offset + edge_size;
    auto const operand_table_size = static_cast<std::uint32_t>(operand_table_size_64);
    auto const operand_data_offset = operand_table_offset + operand_table_size;
    auto const operand_data_size = static_cast<std::uint32_t>(operand_data_size_64);
    auto const string_offset = operand_data_offset + operand_data_size;
    auto const string_size = strings.size();
    auto const footer_offset = string_offset + string_size;
    auto const total_size = static_cast<std::uint32_t>(total_size_64);

    // Get current time for racy-clean detection
    auto now = pup::SystemClock::now();
    auto save_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            now.time_since_epoch()
    )
                            .count();

    // Build header
    auto header = build_header(
        index, strings, file_offset, command_offset, edge_offset, operand_table_offset, operand_data_offset, string_offset, save_time_ns
    );

    // Allocate result buffer
    auto result = Vec<std::byte> {};
    result.resize(total_size);
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

    // Write operand offset table (v8)
    if (!operand_table.empty()) {
        std::memcpy(
            output.subspan(operand_table_offset, operand_table_size).data(),
            operand_table.data(),
            operand_table_size
        );
    }

    // Write operand data (v8)
    if (!operand_data.empty()) {
        std::memcpy(
            output.subspan(operand_data_offset, operand_data_size).data(),
            operand_data.data(),
            operand_data_size
        );
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

} // namespace pup::index
