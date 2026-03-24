// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pup {

class String {
public:
    String();
    String(char const* s);
    String(char const* s, std::size_t len);
    String(std::string_view sv);
    ~String();

    String(String const& other);
    auto operator=(String const& other) -> String&;
    String(String&& other) noexcept;
    auto operator=(String&& other) noexcept -> String&;

    [[nodiscard]]
    auto data() const -> char const*;

    [[nodiscard]]
    auto size() const -> std::size_t;

    [[nodiscard]]
    auto empty() const -> bool;

    [[nodiscard]]
    auto c_str() const -> char const*;

    operator std::string_view() const; // NOLINT(google-explicit-constructor)

    [[nodiscard]]
    auto operator[](std::size_t i) const -> char;

    [[nodiscard]]
    auto back() const -> char;

    auto append(std::string_view sv) -> void;
    auto append(char c) -> void;
    auto operator+=(std::string_view sv) -> String&;
    auto operator+=(char c) -> String&;

    auto reserve(std::size_t n) -> void;
    auto clear() -> void;

    [[nodiscard]]
    auto starts_with(std::string_view sv) const -> bool;

    [[nodiscard]]
    auto ends_with(std::string_view sv) const -> bool;

    [[nodiscard]]
    auto find(char c, std::size_t pos = 0) const -> std::size_t;

    [[nodiscard]]
    auto find(std::string_view sv, std::size_t pos = 0) const -> std::size_t;

    [[nodiscard]]
    auto rfind(char c) const -> std::size_t;

    [[nodiscard]]
    auto substr(std::size_t pos, std::size_t count = npos) const -> String;

    static constexpr auto npos = std::size_t(-1);

private:
    static constexpr std::size_t SSO_CAP = 22;
    static constexpr std::uint8_t HEAP_FLAG = 0x80;

    struct Heap {
        char* ptr;
        std::uint32_t len;
        std::uint32_t cap;
    };

    struct Sso {
        char buf[SSO_CAP + 1]; // +1 for null terminator within SSO_CAP
        std::uint8_t remaining; // SSO_CAP - len; high bit = 0 for SSO
    };

    union {
        Heap heap_;
        Sso sso_;
    };

    static_assert(sizeof(Heap) <= sizeof(Sso));

    [[nodiscard]]
    auto is_heap() const -> bool;
    auto set_sso_size(std::size_t len) -> void;
    auto grow(std::size_t needed) -> void;
};

static_assert(sizeof(String) == 24);

auto operator+(String const& a, std::string_view b) -> String;
auto operator+(std::string_view a, String const& b) -> String;

} // namespace pup
