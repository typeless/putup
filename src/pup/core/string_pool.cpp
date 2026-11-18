// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/string_pool.hpp"
#include "pup/core/string_id.hpp"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

namespace pup {

namespace {

auto fnv1a(std::string_view s) -> std::uint32_t
{
    auto h = std::uint32_t { 2166136261u };
    for (auto c : s) {
        h ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        h *= 16777619u;
    }
    return h;
}

} // namespace

StringPool::StringPool() = default;

StringPool::~StringPool() = default;

StringPool::StringPool(StringPool&& other) noexcept
    : bytes_(std::move(other.bytes_))
    , bytes_size_(std::exchange(other.bytes_size_, 0))
    , entries_(std::move(other.entries_))
    , index_(std::move(other.index_))
{
}

auto StringPool::operator=(StringPool&& other) noexcept -> StringPool&
{
    if (this != &other) {
        bytes_ = std::move(other.bytes_);
        bytes_size_ = std::exchange(other.bytes_size_, 0);
        entries_ = std::move(other.entries_);
        index_ = std::move(other.index_);
    }
    return *this;
}

auto StringPool::key_of(std::uint32_t value) const -> std::string_view
{
    auto const& e = entries_[value - 1];
    return { static_cast<char const*>(bytes_.data()) + e.offset, e.length };
}

auto StringPool::intern(std::string_view str) -> StringId
{
    if (str.empty()) {
        return StringId::Empty;
    }

    auto const h = fnv1a(str);

    if (auto existing = index_.find(h, [&](std::uint32_t v) { return key_of(v) == str; })) {
        return make_string_id(*existing);
    }

    auto const value = static_cast<std::uint32_t>(entries_.size() + 1);
    bytes_.ensure(bytes_size_ + str.size() + 1);
    auto* dst = static_cast<char*>(bytes_.data()) + bytes_size_;
    std::memcpy(dst, str.data(), str.size());
    dst[str.size()] = '\0';
    entries_.emplace_back(Entry { static_cast<std::uint32_t>(bytes_size_), static_cast<std::uint32_t>(str.size()) });
    bytes_size_ += str.size() + 1;

    index_.insert(h, value);

    return make_string_id(value);
}

auto StringPool::get(StringId id) const -> std::string_view
{
    if (is_empty(id)) {
        return {};
    }

    auto const idx = to_underlying(id) - 1;
    if (idx >= entries_.size()) {
        return {};
    }

    auto const& e = entries_[idx];
    return { static_cast<char const*>(bytes_.data()) + e.offset, e.length };
}

auto StringPool::find(std::string_view str) const -> StringId
{
    if (str.empty()) {
        return StringId::Empty;
    }

    auto found = index_.find(fnv1a(str), [&](std::uint32_t v) { return key_of(v) == str; });
    return found ? make_string_id(*found) : StringId::Empty;
}

auto StringPool::size() const -> std::size_t
{
    return entries_.size();
}

auto StringPool::bytes() const -> std::size_t
{
    return bytes_size_ - entries_.size();
}

auto StringPool::clear() -> void
{
    entries_.clear();
    bytes_ = Region {};
    bytes_size_ = 0;
    index_.clear();
}

auto StringPool::reserve(std::size_t count) -> void
{
    index_.reserve(count);
}

} // namespace pup
