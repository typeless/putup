// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/format_to.hpp"
#include "pup/core/string_id.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pup {

class StringPool;

class HeapBuf {
public:
    HeapBuf() = default;
    ~HeapBuf();

    HeapBuf(HeapBuf const&) = delete;
    auto operator=(HeapBuf const&) -> HeapBuf& = delete;
    HeapBuf(HeapBuf&&) noexcept;
    auto operator=(HeapBuf&&) noexcept -> HeapBuf&;

    auto append(std::string_view sv) -> void;
    auto append(char c) -> void;
    auto operator+=(std::string_view sv) -> HeapBuf&;
    auto operator+=(char c) -> HeapBuf&;

    auto reserve(std::size_t n) -> void;
    auto resize(std::size_t n) -> void;
    auto clear() -> void;

    [[nodiscard]] auto data() const -> char const* { return data_; }
    [[nodiscard]] auto data() -> char* { return data_; }
    [[nodiscard]] auto size() const -> std::size_t { return size_; }
    [[nodiscard]] auto empty() const -> bool { return size_ == 0; }
    [[nodiscard]] auto c_str() const -> char const* { return data_ ? data_ : ""; }
    [[nodiscard]] auto view() const -> std::string_view { return { data_ ? data_ : "", size_ }; }

    [[nodiscard]] auto intern(StringPool& pool) const -> StringId;

    auto fmt(std::string_view pattern) -> void;

    template<typename... Args>
    auto fmt(std::string_view pattern, Args const&... args) -> void
    {
        FormatArg arg_array[] = { FormatArg(args)... };
        format_to(*this, pattern, arg_array, sizeof...(Args));
    }

private:
    char* data_ = nullptr;
    std::uint32_t size_ = 0;
    std::uint32_t capacity_ = 0;

    auto grow(std::size_t needed) -> void;
};

} // namespace pup
