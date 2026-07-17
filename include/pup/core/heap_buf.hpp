// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/format_to.hpp"
#include "pup/core/region.hpp"
#include "pup/core/string_id.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pup {

class StringPool;

class HeapBuf final {
public:
    HeapBuf() = default;
    ~HeapBuf();

    HeapBuf(HeapBuf const&) = delete;
    auto operator=(HeapBuf const&) -> HeapBuf& = delete;
    HeapBuf(HeapBuf&& other) noexcept;
    auto operator=(HeapBuf&& other) noexcept -> HeapBuf&;

    auto append(std::string_view sv) -> void;
    auto append(char c) -> void;
    auto operator+=(std::string_view sv) -> HeapBuf&;
    auto operator+=(char c) -> HeapBuf&;

    auto reserve(std::size_t n) -> void;
    auto resize(std::size_t n) -> void;
    auto clear() -> void;

    [[nodiscard]]
    auto data() const -> char const*
    {
        return static_cast<char const*>(region_.data());
    }
    [[nodiscard]]
    auto data() -> char*
    {
        return static_cast<char*>(region_.data());
    }
    [[nodiscard]]
    auto size() const -> std::size_t
    {
        return size_;
    }
    [[nodiscard]]
    auto empty() const -> bool
    {
        return size_ == 0;
    }
    [[nodiscard]]
    auto c_str() const -> char const*
    {
        return data() ? data() : "";
    }
    [[nodiscard]]
    auto view() const -> std::string_view
    {
        return { data() ? data() : "", size_ };
    }

    [[nodiscard]]
    auto intern(StringPool& pool) const -> StringId;

    auto fmt(std::string_view pattern) -> void;

    template<typename... Args>
    auto fmt(std::string_view pattern, Args const&... args) -> void
    {
        FormatArg arg_array[] = { FormatArg(args)... };
        format_to(*this, pattern, arg_array, sizeof...(Args));
    }

private:
    static constexpr std::size_t SPILL_RESERVE = std::size_t { 1 } << 24;
    Region region_;
    std::uint32_t size_ = 0;

    auto grow(std::size_t needed) -> void;
};

} // namespace pup
