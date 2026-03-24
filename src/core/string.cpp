// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/string.hpp"

#include <cstdlib>
#include <cstring>

namespace pup {

// =============================================================================
// SSO helpers
// =============================================================================

auto String::is_heap() const -> bool
{
    return (sso_.remaining & HEAP_FLAG) != 0;
}

auto String::set_sso_size(std::size_t len) -> void
{
    sso_.remaining = static_cast<std::uint8_t>(SSO_CAP - len);
    sso_.buf[len] = '\0';
}

// =============================================================================
// Construction
// =============================================================================

String::String()
{
    sso_.buf[0] = '\0';
    sso_.remaining = SSO_CAP;
}

String::String(char const* s)
    : String(std::string_view { s })
{
}

String::String(char const* s, std::size_t len)
    : String(std::string_view { s, len })
{
}

String::String(std::string_view sv)
{
    if (sv.size() <= SSO_CAP) {
        std::memcpy(sso_.buf, sv.data(), sv.size());
        set_sso_size(sv.size());
    } else {
        auto cap = sv.size() + 1;
        heap_.ptr = static_cast<char*>(std::malloc(cap));
        std::memcpy(heap_.ptr, sv.data(), sv.size());
        heap_.ptr[sv.size()] = '\0';
        heap_.len = static_cast<std::uint32_t>(sv.size());
        heap_.cap = static_cast<std::uint32_t>(cap);
        sso_.remaining = HEAP_FLAG;
    }
}

String::~String()
{
    if (is_heap()) {
        std::free(heap_.ptr);
    }
}

// =============================================================================
// Copy & move
// =============================================================================

String::String(String const& other)
{
    if (other.is_heap()) {
        auto cap = other.heap_.len + 1;
        heap_.ptr = static_cast<char*>(std::malloc(cap));
        std::memcpy(heap_.ptr, other.heap_.ptr, other.heap_.len + 1);
        heap_.len = other.heap_.len;
        heap_.cap = static_cast<std::uint32_t>(cap);
        sso_.remaining = HEAP_FLAG;
    } else {
        std::memcpy(&sso_, &other.sso_, sizeof(Sso));
    }
}

auto String::operator=(String const& other) -> String&
{
    if (this != &other) {
        if (is_heap()) {
            std::free(heap_.ptr);
        }
        if (other.is_heap()) {
            auto cap = other.heap_.len + 1;
            heap_.ptr = static_cast<char*>(std::malloc(cap));
            std::memcpy(heap_.ptr, other.heap_.ptr, other.heap_.len + 1);
            heap_.len = other.heap_.len;
            heap_.cap = static_cast<std::uint32_t>(cap);
            sso_.remaining = HEAP_FLAG;
        } else {
            std::memcpy(&sso_, &other.sso_, sizeof(Sso));
        }
    }
    return *this;
}

String::String(String&& other) noexcept
{
    std::memcpy(&sso_, &other.sso_, sizeof(Sso));
    other.sso_.buf[0] = '\0';
    other.sso_.remaining = SSO_CAP;
}

auto String::operator=(String&& other) noexcept -> String&
{
    if (this != &other) {
        if (is_heap()) {
            std::free(heap_.ptr);
        }
        std::memcpy(&sso_, &other.sso_, sizeof(Sso));
        other.sso_.buf[0] = '\0';
        other.sso_.remaining = SSO_CAP;
    }
    return *this;
}

// =============================================================================
// Accessors
// =============================================================================

auto String::data() const -> char const*
{
    return is_heap() ? heap_.ptr : sso_.buf;
}

auto String::size() const -> std::size_t
{
    return is_heap() ? heap_.len : (SSO_CAP - sso_.remaining);
}

auto String::empty() const -> bool
{
    return size() == 0;
}

auto String::c_str() const -> char const*
{
    return data();
}

String::operator std::string_view() const
{
    return { data(), size() };
}

auto String::operator[](std::size_t i) const -> char
{
    return data()[i];
}

auto String::back() const -> char
{
    return data()[size() - 1];
}

// =============================================================================
// Mutation
// =============================================================================

auto String::grow(std::size_t needed) -> void
{
    if (is_heap()) {
        if (needed < heap_.cap) {
            return;
        }
        auto new_cap = heap_.cap;
        while (new_cap < needed) {
            new_cap = new_cap + new_cap / 2 + 16;
        }
        heap_.ptr = static_cast<char*>(std::realloc(heap_.ptr, new_cap));
        heap_.cap = static_cast<std::uint32_t>(new_cap);
    } else {
        // SSO → heap transition
        auto old_len = size();
        auto new_cap = static_cast<std::uint32_t>(needed + needed / 2 + 16);
        if (new_cap < static_cast<std::uint32_t>(needed)) {
            new_cap = static_cast<std::uint32_t>(needed);
        }
        auto* buf = static_cast<char*>(std::malloc(new_cap));
        std::memcpy(buf, sso_.buf, old_len + 1);
        heap_.ptr = buf;
        heap_.len = static_cast<std::uint32_t>(old_len);
        heap_.cap = new_cap;
        sso_.remaining = HEAP_FLAG;
    }
}

auto String::append(std::string_view sv) -> void
{
    if (sv.empty()) {
        return;
    }
    auto old_len = size();
    auto new_len = old_len + sv.size();
    if (is_heap()) {
        if (new_len + 1 > heap_.cap) {
            grow(new_len + 1);
        }
        std::memcpy(heap_.ptr + old_len, sv.data(), sv.size());
        heap_.ptr[new_len] = '\0';
        heap_.len = static_cast<std::uint32_t>(new_len);
    } else if (new_len <= SSO_CAP) {
        std::memcpy(sso_.buf + old_len, sv.data(), sv.size());
        set_sso_size(new_len);
    } else {
        grow(new_len + 1);
        std::memcpy(heap_.ptr + old_len, sv.data(), sv.size());
        heap_.ptr[new_len] = '\0';
        heap_.len = static_cast<std::uint32_t>(new_len);
    }
}

auto String::append(char c) -> void
{
    append(std::string_view { &c, 1 });
}

auto String::operator+=(std::string_view sv) -> String&
{
    append(sv);
    return *this;
}

auto String::operator+=(char c) -> String&
{
    append(c);
    return *this;
}

auto String::reserve(std::size_t n) -> void
{
    if (n + 1 > SSO_CAP && (!is_heap() || n + 1 > heap_.cap)) {
        grow(n + 1);
    }
}

auto String::clear() -> void
{
    if (is_heap()) {
        std::free(heap_.ptr);
    }
    sso_.buf[0] = '\0';
    sso_.remaining = SSO_CAP;
}

// =============================================================================
// Searching
// =============================================================================

auto String::starts_with(std::string_view sv) const -> bool
{
    return std::string_view { *this }.starts_with(sv);
}

auto String::ends_with(std::string_view sv) const -> bool
{
    return std::string_view { *this }.ends_with(sv);
}

auto String::find(char c, std::size_t pos) const -> std::size_t
{
    auto sv = std::string_view { *this };
    auto result = sv.find(c, pos);
    return result == std::string_view::npos ? npos : result;
}

auto String::find(std::string_view sv, std::size_t pos) const -> std::size_t
{
    auto self = std::string_view { *this };
    auto result = self.find(sv, pos);
    return result == std::string_view::npos ? npos : result;
}

auto String::rfind(char c) const -> std::size_t
{
    auto sv = std::string_view { *this };
    auto result = sv.rfind(c);
    return result == std::string_view::npos ? npos : result;
}

auto String::substr(std::size_t pos, std::size_t count) const -> String
{
    auto sv = std::string_view { *this };
    if (pos >= sv.size()) {
        return {};
    }
    return String { sv.substr(pos, count) };
}

// =============================================================================
// Free functions
// =============================================================================

auto operator+(String const& a, std::string_view b) -> String
{
    auto result = String { a };
    result += b;
    return result;
}

auto operator+(std::string_view a, String const& b) -> String
{
    auto result = String { a };
    result += std::string_view { b };
    return result;
}

} // namespace pup
