// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/robin_hood_index.hpp"

#include <cstdint>
#include <utility>

namespace pup {

namespace {

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

auto RobinHoodIndex::probe_insert(std::uint32_t h, std::uint32_t value) -> void
{
    auto const mask = capacity_ - 1;
    auto slot = static_cast<std::size_t>(h) & mask;
    auto disp = std::uint16_t { 0 };

    for (;;) {
        if (meta()[slot].hash == 0) {
            meta()[slot] = { h, disp };
            values()[slot] = value;
            return;
        }
        if (meta()[slot].displacement < disp) {
            std::swap(h, meta()[slot].hash);
            std::swap(disp, meta()[slot].displacement);
            std::swap(value, values()[slot]);
        }
        ++disp;
        slot = (slot + 1) & mask;
    }
}

auto RobinHoodIndex::rebuild(std::size_t new_cap) -> void
{
    auto old_slots = std::move(slots_);
    auto const old_cap = capacity_;
    auto const* old_meta = static_cast<Meta const*>(old_slots.data());
    auto const* old_values = reinterpret_cast<std::uint32_t const*>(old_meta + old_cap);

    slots_ = Region {};
    slots_.ensure(new_cap * (sizeof(Meta) + sizeof(std::uint32_t)));
    capacity_ = new_cap;

    for (auto i = std::size_t { 0 }; i < old_cap; ++i) {
        if (old_meta[i].hash >= 2) {
            probe_insert(old_meta[i].hash, old_values[i]);
        }
    }
}

auto RobinHoodIndex::insert(std::uint32_t hash, std::uint32_t value) -> void
{
    if (count_ >= capacity_ * 4 / 5) {
        rebuild(capacity_ == 0 ? 16 : capacity_ * 2);
    }
    probe_insert(fix_hash(hash), value);
    ++count_;
}

auto RobinHoodIndex::reserve(std::size_t count) -> void
{
    auto needed = count * 5 / 4 + 1;
    auto cap = next_power_of_two(needed < 16 ? 16 : needed);

    if (cap <= capacity_) {
        return;
    }

    rebuild(cap);
}

auto RobinHoodIndex::clear() -> void
{
    slots_ = Region {};
    capacity_ = 0;
    count_ = 0;
}

} // namespace pup
