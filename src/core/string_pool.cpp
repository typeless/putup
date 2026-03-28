// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/string_pool.hpp"

#include <cstdlib>
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

auto fix_hash(std::uint32_t h) -> std::uint32_t
{
    return h < 2 ? h + 2 : h;
}

auto next_power_of_two(std::size_t n) -> std::size_t
{
    auto v = n - 1;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
}

} // namespace

StringPool::StringPool() = default;

StringPool::~StringPool()
{
    std::free(meta_);
    std::free(values_);
}

StringPool::StringPool(StringPool&& other) noexcept
    : storage_(std::move(other.storage_))
    , meta_(std::exchange(other.meta_, nullptr))
    , values_(std::exchange(other.values_, nullptr))
    , index_capacity_(std::exchange(other.index_capacity_, 0))
    , index_count_(std::exchange(other.index_count_, 0))
{
}

auto StringPool::operator=(StringPool&& other) noexcept -> StringPool&
{
    if (this != &other) {
        std::free(meta_);
        std::free(values_);

        storage_ = std::move(other.storage_);
        meta_ = std::exchange(other.meta_, nullptr);
        values_ = std::exchange(other.values_, nullptr);
        index_capacity_ = std::exchange(other.index_capacity_, 0);
        index_count_ = std::exchange(other.index_count_, 0);
    }
    return *this;
}

auto StringPool::key_at(std::size_t slot) const -> std::string_view
{
    return storage_[to_underlying(values_[slot]) - 1].view();
}

auto StringPool::probe_find(std::uint32_t h, std::string_view key) const -> StringId
{
    if (index_capacity_ == 0) {
        return StringId::Empty;
    }

    auto mask = index_capacity_ - 1;
    auto slot = static_cast<std::size_t>(h) & mask;
    auto disp = std::uint16_t { 0 };

    for (;;) {
        if (meta_[slot].hash == 0) {
            return StringId::Empty;
        }
        if (meta_[slot].displacement < disp) {
            return StringId::Empty;
        }
        if (meta_[slot].hash == h && key_at(slot) == key) {
            return values_[slot];
        }
        ++disp;
        slot = (slot + 1) & mask;
    }
}

auto StringPool::probe_insert(std::uint32_t h, StringId id) -> void
{
    auto mask = index_capacity_ - 1;
    auto slot = static_cast<std::size_t>(h) & mask;
    auto disp = std::uint16_t { 0 };

    for (;;) {
        if (meta_[slot].hash == 0) {
            meta_[slot] = { h, disp };
            values_[slot] = id;
            return;
        }
        if (meta_[slot].displacement < disp) {
            std::swap(h, meta_[slot].hash);
            std::swap(disp, meta_[slot].displacement);
            std::swap(id, values_[slot]);
        }
        ++disp;
        slot = (slot + 1) & mask;
    }
}

auto StringPool::grow() -> void
{
    auto new_cap = index_capacity_ == 0 ? std::size_t { 16 } : index_capacity_ * 2;

    auto* old_meta = meta_;
    auto* old_values = values_;
    auto old_cap = index_capacity_;

    meta_ = static_cast<Meta*>(std::calloc(new_cap, sizeof(Meta)));
    values_ = static_cast<StringId*>(std::malloc(new_cap * sizeof(StringId)));
    if (!meta_ || !values_) {
        std::abort();
    }
    index_capacity_ = new_cap;

    for (auto i = std::size_t { 0 }; i < old_cap; ++i) {
        if (old_meta[i].hash >= 2) {
            probe_insert(old_meta[i].hash, old_values[i]);
        }
    }

    std::free(old_meta);
    std::free(old_values);
}

auto StringPool::intern(std::string_view str) -> StringId
{
    if (str.empty()) {
        return StringId::Empty;
    }

    auto h = fix_hash(fnv1a(str));

    if (auto existing = probe_find(h, str); !is_empty(existing)) {
        return existing;
    }

    auto const id = make_string_id(static_cast<std::uint32_t>(storage_.size() + 1));
    auto& slot = storage_.emplace_back();
    slot.append(str);

    if (index_count_ >= index_capacity_ * 4 / 5) {
        grow();
    }

    probe_insert(h, id);
    ++index_count_;

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

    return storage_[idx].view();
}

auto StringPool::find(std::string_view str) const -> StringId
{
    if (str.empty()) {
        return StringId::Empty;
    }

    return probe_find(fix_hash(fnv1a(str)), str);
}

auto StringPool::size() const -> std::size_t
{
    return storage_.size();
}

auto StringPool::bytes() const -> std::size_t
{
    auto total = std::size_t { 0 };
    for (auto i = std::size_t { 0 }; i < storage_.size(); ++i) {
        total += storage_[i].size();
    }
    return total;
}

auto StringPool::clear() -> void
{
    storage_.clear();
    std::free(meta_);
    std::free(values_);
    meta_ = nullptr;
    values_ = nullptr;
    index_capacity_ = 0;
    index_count_ = 0;
}

auto StringPool::reserve(std::size_t count) -> void
{
    auto needed = count * 5 / 4 + 1;
    auto cap = next_power_of_two(needed < 16 ? 16 : needed);

    if (cap <= index_capacity_) {
        return;
    }

    auto* old_meta = meta_;
    auto* old_values = values_;
    auto old_cap = index_capacity_;

    meta_ = static_cast<Meta*>(std::calloc(cap, sizeof(Meta)));
    values_ = static_cast<StringId*>(std::malloc(cap * sizeof(StringId)));
    if (!meta_ || !values_) {
        std::abort();
    }
    index_capacity_ = cap;

    for (auto i = std::size_t { 0 }; i < old_cap; ++i) {
        if (old_meta[i].hash >= 2) {
            probe_insert(old_meta[i].hash, old_values[i]);
        }
    }

    std::free(old_meta);
    std::free(old_values);
}

} // namespace pup
