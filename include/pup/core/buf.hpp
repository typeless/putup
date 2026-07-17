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

class Buf final {
public:
    Buf() = default;

    Buf(Buf const&) = delete;
    auto operator=(Buf const&) -> Buf& = delete;
    Buf(Buf&&) = delete;
    auto operator=(Buf&&) -> Buf& = delete;

    auto append(std::string_view sv) -> void;
    auto append(char c) -> void;
    auto operator+=(std::string_view sv) -> Buf&;
    auto operator+=(char c) -> Buf&;

    auto reserve(std::size_t n) -> void;
    auto clear() -> void;

    [[nodiscard]]
    auto data() const -> char const*
    {
        return data_;
    }
    [[nodiscard]]
    auto data() -> char*
    {
        return data_;
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
    auto c_str() const -> char const*;
    [[nodiscard]]
    auto view() const -> std::string_view
    {
        return { data_, size_ };
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
    static constexpr std::uint32_t INLINE_CAP = 4096;
    static constexpr std::size_t SPILL_RESERVE = std::size_t { 1 } << 24;
    char buf_[INLINE_CAP] = {};
    char* data_ = buf_;
    std::uint32_t size_ = 0;
    std::uint32_t capacity_ = INLINE_CAP;
    Region region_;

    auto is_spilled() const -> bool { return data_ != buf_; }
    auto grow(std::size_t needed) -> void;
};

} // namespace pup
