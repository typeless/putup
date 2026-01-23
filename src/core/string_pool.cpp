// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/string_pool.hpp"

namespace pup {

StringPool::StringPool() = default;
StringPool::~StringPool() = default;
StringPool::StringPool(StringPool&&) noexcept = default;
auto StringPool::operator=(StringPool&&) noexcept -> StringPool& = default;

auto StringPool::intern(std::string_view str) -> StringId
{
    if (str.empty()) {
        return StringId::Empty;
    }

    if (auto it = index_.find(str); it != index_.end()) {
        return it->second;
    }

    auto const id = make_string_id(static_cast<std::uint32_t>(storage_.size() + 1));

    storage_.emplace_back(str);
    auto const& stored = storage_.back();
    index_.emplace(std::string_view { stored }, id);

    return id;
}

auto StringPool::get(StringId id) const -> std::string_view
{
    if (is_empty(id)) {
        return {};
    }

    auto const idx = to_underlying(id) - 1;
    if (idx >= storage_.size()) {
        return {};
    }

    return storage_[idx];
}

auto StringPool::find(std::string_view str) const -> StringId
{
    if (str.empty()) {
        return StringId::Empty;
    }

    if (auto it = index_.find(str); it != index_.end()) {
        return it->second;
    }

    return StringId::Empty;
}

auto StringPool::size() const -> std::size_t
{
    return storage_.size();
}

auto StringPool::clear() -> void
{
    storage_.clear();
    index_.clear();
}

auto StringPool::reserve(std::size_t count) -> void
{
    index_.reserve(count);
}

} // namespace pup
