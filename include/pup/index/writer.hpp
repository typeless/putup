// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "entry.hpp"
#include "format.hpp"
#include "pup/core/result.hpp"

#include <filesystem>
#include <unordered_map>
#include <vector>

namespace pup::index {

/// Index file writer with atomic write support
class IndexWriter {
public:
    IndexWriter() = default;

    /// Write an index to a file atomically
    /// Uses a temporary file and rename for atomic operation
    [[nodiscard]] auto write(
        std::filesystem::path const& path,
        Index const& index
    ) -> Result<void>;

    /// Serialize an index to a byte vector
    [[nodiscard]] auto serialize(Index const& index) -> Result<std::vector<std::byte>>;

private:
    /// String table builder
    class StringTable {
    public:
        /// Add a string and return its offset
        auto add(std::string_view str) -> std::uint32_t;

        /// Get the table data
        [[nodiscard]] auto data() const -> std::vector<char> const& { return data_; }

        /// Get the current size
        [[nodiscard]] auto size() const -> std::uint32_t
        {
            return static_cast<std::uint32_t>(data_.size());
        }

    private:
        std::vector<char> data_ = {};
        std::unordered_map<std::string, std::uint32_t> offsets_ = {};
    };

    auto build_header(
        Index const& index,
        StringTable const& strings,
        std::uint64_t file_offset,
        std::uint64_t command_offset,
        std::uint64_t edge_offset,
        std::uint64_t string_offset
    ) -> RawHeader;
};

} // namespace pup::index
