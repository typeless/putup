// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#pragma once

#include <cstdint>
#include <optional>

namespace tiled {

class StackCursor final {
public:
    [[nodiscard]]
    static auto make(std::uint64_t reserved, std::uint64_t page) -> std::optional<StackCursor>;

    [[nodiscard]]
    auto push(std::uint64_t bytes, std::uint64_t align) -> std::optional<std::uint64_t>;

    [[nodiscard]]
    auto extend_top(std::uint64_t offset, std::uint64_t old_bytes, std::uint64_t new_bytes) -> bool;

    auto pop_top(std::uint64_t offset, std::uint64_t bytes) -> void;

    auto decommit_above(std::uint64_t keep) -> void;

    [[nodiscard]]
    auto top() const -> std::uint64_t;

    [[nodiscard]]
    auto committed() const -> std::uint64_t;

private:
    StackCursor(std::uint64_t reserved, std::uint64_t page);

    auto raise_commit_to(std::uint64_t end) -> void;

    std::uint64_t top_ = 0;
    std::uint64_t committed_ = 0;
    std::uint64_t reserved_;
    std::uint64_t page_;
};

} // namespace tiled
