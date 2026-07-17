// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/string_pool.hpp"
#include "pup/core/string_id.hpp"

#include <cstdint>
#include <cstdlib>
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

StringPool::~StringPool() = default;

StringPool::StringPool(StringPool&& other) noexcept
    : storage_(std::move(other.storage_))
    , index_(std::move(other.index_))
    , index_capacity_(std::exchange(other.index_capacity_, 0))
    , index_count_(std::exchange(other.index_count_, 0))
{
}

auto StringPool::operator=(StringPool&& other) noexcept -> StringPool&
{
    if (this != &other) {
        storage_ = std::move(other.storage_);
        index_ = std::move(other.index_);
        index_capacity_ = std::exchange(other.index_capacity_, 0);
        index_count_ = std::exchange(other.index_count_, 0);
    }
    return *this;
}

auto StringPool::meta() const -> Meta*
{
    return static_cast<Meta*>(index_.data());
}

auto StringPool::values() const -> StringId*
{
    return reinterpret_cast<StringId*>(meta() + index_capacity_);
}

auto StringPool::key_at(std::size_t slot) const -> std::string_view
{
    return storage_[to_underlying(values()[slot]) - 1].view();
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
        if (meta()[slot].hash == 0) {
            return StringId::Empty;
        }
        if (meta()[slot].displacement < disp) {
            return StringId::Empty;
        }
        if (meta()[slot].hash == h && key_at(slot) == key) {
            return values()[slot];
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
        if (meta()[slot].hash == 0) {
            meta()[slot] = { h, disp };
            values()[slot] = id;
            return;
        }
        if (meta()[slot].displacement < disp) {
            std::swap(h, meta()[slot].hash);
            std::swap(disp, meta()[slot].displacement);
            std::swap(id, values()[slot]);
        }
        ++disp;
        slot = (slot + 1) & mask;
    }
}

auto StringPool::rebuild(std::size_t new_cap) -> void
{
    auto old_index = std::move(index_);
    auto const old_cap = index_capacity_;
    auto const* old_meta = static_cast<Meta const*>(old_index.data());
    auto const* old_values = reinterpret_cast<StringId const*>(old_meta + old_cap);

    index_ = Region {};
    index_.ensure(new_cap * (sizeof(Meta) + sizeof(StringId)));
    index_capacity_ = new_cap;

    for (auto i = std::size_t { 0 }; i < old_cap; ++i) {
        if (old_meta[i].hash >= 2) {
            probe_insert(old_meta[i].hash, old_values[i]);
        }
    }
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
        rebuild(index_capacity_ == 0 ? 16 : index_capacity_ * 2);
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
    index_ = Region {};
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

    rebuild(cap);
}

} // namespace pup
