// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#pragma once

#include "tiled/layout.hpp"
#include "tiled/platform.hpp"
#include "tiled/pool.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

namespace tiled {

template<Layout L>
requires(is_valid(L))
class SpaceStorage final {
public:
    constexpr SpaceStorage() = default;
    SpaceStorage(SpaceStorage const&) = delete;
    auto operator=(SpaceStorage const&) -> SpaceStorage& = delete;
    SpaceStorage(SpaceStorage&&) = delete;
    auto operator=(SpaceStorage&&) -> SpaceStorage& = delete;

    [[nodiscard]]
    auto pool() -> Pool&
    {
        if (!pool_) {
            realize();
        }
        return *pool_;
    }

private:
    [[noreturn]]
    static auto fail(char const* what, std::uint64_t value) -> void
    {
        char message[128];
        auto const what_length = std::strlen(what);
        std::memcpy(message, what, what_length);
        auto* end = message + what_length;
        *end++ = '0';
        *end++ = 'x';
        end = std::to_chars(end, message + sizeof(message), value, 16).ptr;
        fatal(message, static_cast<std::size_t>(end - message));
    }

    auto realize() -> void
    {
        auto const page = page_size();
        if ((std::uint64_t { 1 } << L.min_tile_log2) < page) {
            fail("tiled: the smallest tile is below the page size ", page);
        }
        auto const image = reinterpret_cast<std::uintptr_t>(&page_size);
        auto in_image_node = false;
        for (auto const& node : L.image_nodes) {
            in_image_node = in_image_node || (image >= node.base && image - node.base < (std::uint64_t { 1 } << node.log2));
        }
        if (!in_image_node) {
            fail("tiled: the image lies outside the declared image nodes, at ", image);
        }
        if (!reserve_at(reinterpret_cast<void*>(L.base), std::size_t { 1 } << L.pool_log2)) {
            fail("tiled: the OS refused the declared pool node of the layout, at ", L.base);
        }
        auto made = Pool::make(metadata_, L.base, L.pool_log2, L.min_tile_log2);
        if (!made) {
            fail("tiled: the layout's pool cannot be made, at ", L.base);
        }
        pool_.emplace(std::move(*made));
    }

    std::optional<Pool> pool_;
    std::uint8_t metadata_[Pool::metadata_bytes(L.pool_log2, L.min_tile_log2)] {};
};

} // namespace tiled
