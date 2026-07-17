// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/id_array.hpp"

#include <cstdint>
#include <cstring>

namespace pup {

auto IdArray32::data() const -> std::uint32_t*
{
    return static_cast<std::uint32_t*>(region_.data());
}

auto IdArray32::capacity() const -> std::size_t
{
    return region_.committed() / sizeof(std::uint32_t);
}

auto IdArray32::resize(std::uint32_t max_id) -> void
{
    auto const needed = static_cast<std::size_t>(max_id) + 1;
    region_.ensure(needed * sizeof(std::uint32_t));
    present_.resize(max_id);
}

auto IdArray32::set(std::uint32_t id, std::uint32_t value) -> void
{
    if (static_cast<std::size_t>(id) >= capacity()) {
        resize(id);
    }
    data()[id] = value;
    present_.insert(id);
}

auto IdArray32::get(std::uint32_t id) const -> std::uint32_t
{
    if (static_cast<std::size_t>(id) >= capacity()) {
        return 0;
    }
    return data()[id];
}

auto IdArray32::contains(std::uint32_t id) const -> bool
{
    return present_.contains(id);
}

auto IdArray32::remove(std::uint32_t id) -> void
{
    present_.remove(id);
}

auto IdArray32::clear() -> void
{
    if (data()) {
        std::memset(data(), 0, region_.committed());
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

    auto inner = Context { data(), fn, ctx };
    present_.for_each(
        [](std::uint32_t id, void* raw) {
            auto const* c = static_cast<Context const*>(raw);
            c->fn(id, c->data[id], c->ctx);
        },
        &inner
    );
}

} // namespace pup
