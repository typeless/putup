// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/id_array.hpp"

#include <cstdlib>
#include <cstring>
#include <utility>

namespace pup {

// --- IdArray32 ---

IdArray32::~IdArray32()
{
    std::free(data_);
}

IdArray32::IdArray32(IdArray32&& other) noexcept
    : data_(std::exchange(other.data_, nullptr))
    , capacity_(std::exchange(other.capacity_, 0))
    , present_(std::move(other.present_))
{
}

auto IdArray32::operator=(IdArray32&& other) noexcept -> IdArray32&
{
    if (this != &other) {
        std::free(data_);
        data_ = std::exchange(other.data_, nullptr);
        capacity_ = std::exchange(other.capacity_, 0);
        present_ = std::move(other.present_);
    }
    return *this;
}

auto IdArray32::resize(std::uint32_t max_id) -> void
{
    auto const needed = static_cast<std::size_t>(max_id) + 1;
    if (needed <= capacity_) {
        return;
    }
    auto* p = static_cast<std::uint32_t*>(std::realloc(data_, needed * sizeof(std::uint32_t)));
    if (!p) {
        return;
    }
    std::memset(p + capacity_, 0, (needed - capacity_) * sizeof(std::uint32_t));
    data_ = p;
    capacity_ = needed;
    present_.resize(max_id);
}

auto IdArray32::set(std::uint32_t id, std::uint32_t value) -> void
{
    if (static_cast<std::size_t>(id) >= capacity_) {
        resize(id);
    }
    data_[id] = value;
    present_.insert(id);
}

auto IdArray32::get(std::uint32_t id) const -> std::uint32_t
{
    if (static_cast<std::size_t>(id) >= capacity_) {
        return 0;
    }
    return data_[id];
}

auto IdArray32::contains(std::uint32_t id) const -> bool
{
    return present_.contains(id);
}

auto IdArray32::clear() -> void
{
    if (data_) {
        std::memset(data_, 0, capacity_ * sizeof(std::uint32_t));
    }
    present_.clear();
}

auto IdArray32::for_each(void (*fn)(std::uint32_t id, std::uint32_t value, void* ctx), void* ctx) const -> void
{
    struct Context {
        std::uint32_t const* data;
        void (*fn)(std::uint32_t, std::uint32_t, void*);
        void* ctx;
    };

    auto inner = Context { data_, fn, ctx };
    present_.for_each(
        [](std::uint32_t id, void* raw) {
            auto const* c = static_cast<Context const*>(raw);
            c->fn(id, c->data[id], c->ctx);
        },
        &inner
    );
}

// --- IdArray64 ---

IdArray64::~IdArray64()
{
    std::free(data_);
}

IdArray64::IdArray64(IdArray64&& other) noexcept
    : data_(std::exchange(other.data_, nullptr))
    , capacity_(std::exchange(other.capacity_, 0))
    , present_(std::move(other.present_))
{
}

auto IdArray64::operator=(IdArray64&& other) noexcept -> IdArray64&
{
    if (this != &other) {
        std::free(data_);
        data_ = std::exchange(other.data_, nullptr);
        capacity_ = std::exchange(other.capacity_, 0);
        present_ = std::move(other.present_);
    }
    return *this;
}

auto IdArray64::resize(std::uint32_t max_id) -> void
{
    auto const needed = static_cast<std::size_t>(max_id) + 1;
    if (needed <= capacity_) {
        return;
    }
    auto* p = static_cast<std::uint64_t*>(std::realloc(data_, needed * sizeof(std::uint64_t)));
    if (!p) {
        return;
    }
    std::memset(p + capacity_, 0, (needed - capacity_) * sizeof(std::uint64_t));
    data_ = p;
    capacity_ = needed;
    present_.resize(max_id);
}

auto IdArray64::set(std::uint32_t id, std::uint64_t value) -> void
{
    if (static_cast<std::size_t>(id) >= capacity_) {
        resize(id);
    }
    data_[id] = value;
    present_.insert(id);
}

auto IdArray64::get(std::uint32_t id) const -> std::uint64_t
{
    if (static_cast<std::size_t>(id) >= capacity_) {
        return 0;
    }
    return data_[id];
}

auto IdArray64::contains(std::uint32_t id) const -> bool
{
    return present_.contains(id);
}

auto IdArray64::clear() -> void
{
    if (data_) {
        std::memset(data_, 0, capacity_ * sizeof(std::uint64_t));
    }
    present_.clear();
}

auto IdArray64::for_each(void (*fn)(std::uint32_t id, std::uint64_t value, void* ctx), void* ctx) const -> void
{
    struct Context {
        std::uint64_t const* data;
        void (*fn)(std::uint32_t, std::uint64_t, void*);
        void* ctx;
    };

    auto inner = Context { data_, fn, ctx };
    present_.for_each(
        [](std::uint32_t id, void* raw) {
            auto const* c = static_cast<Context const*>(raw);
            c->fn(id, c->data[id], c->ctx);
        },
        &inner
    );
}

} // namespace pup
