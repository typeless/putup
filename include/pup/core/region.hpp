// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstddef>

namespace pup {

// Contiguous growable memory on a reserved virtual range: the base address
// never moves, growth commits zero-filled pages, shrink returns pages to
// the OS. Reserves lazily on first ensure; shrink(0) releases everything.
class Region final {
public:
    static constexpr auto DEFAULT_RESERVE = std::size_t { 1 } << 30;

    Region() = default;
    explicit Region(std::size_t reserve_bytes);
    ~Region();

    Region(Region const&) = delete;
    auto operator=(Region const&) -> Region& = delete;

    Region(Region&&) noexcept;
    auto operator=(Region&&) noexcept -> Region&;

    auto ensure(std::size_t bytes) -> void;
    auto shrink(std::size_t keep_bytes) -> void;

    [[nodiscard]]
    auto data() const -> void*;

    [[nodiscard]]
    auto committed() const -> std::size_t;

private:
    void* base_ = nullptr;
    std::size_t reserved_ = 0;
    std::size_t committed_ = 0;
    std::size_t reserve_bytes_ = DEFAULT_RESERVE;
};

} // namespace pup
