// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/path_pool.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/vec.hpp"

#include <cassert>
#include <cstdint>
#include <string_view>

namespace pup {

PathPool::PathPool()
{
    entries_.push_back(Entry {});
    children_.resize(1);
}

PathPool::~PathPool() = default;
PathPool::PathPool(PathPool&&) noexcept = default;
auto PathPool::operator=(PathPool&&) noexcept -> PathPool& = default;

auto PathPool::intern(PathId parent, StringId name) -> PathId
{
    if (is_empty(name)) {
        return parent;
    }

    auto const parent_idx = to_underlying(parent);
    assert(parent_idx < children_.size());

    auto const name_key = to_underlying(name);
    auto const* found = children_[parent_idx].find(name_key);
    if (found) {
        return make_path_id(*found);
    }

    auto const idx = static_cast<std::uint32_t>(entries_.size());
    auto const id = make_path_id(idx);

    entries_.push_back(Entry { .parent = parent, .name = name });
    children_.resize(idx + 1);
    children_[parent_idx].insert(name_key, idx);

    return id;
}

auto PathPool::intern_path(std::string_view path, StringPool& pool) -> PathId
{
    auto current = PathId::Root;
    auto remaining = path;

    while (!remaining.empty()) {
        auto slash = remaining.find('/');
        auto comp = (slash == std::string_view::npos) ? remaining : remaining.substr(0, slash);
        remaining = (slash == std::string_view::npos) ? std::string_view {} : remaining.substr(slash + 1);

        if (comp.empty() || comp == ".") {
            continue;
        }

        current = intern(current, pool.intern(comp));
    }

    return current;
}

auto PathPool::parent(PathId id) const -> PathId
{
    assert(to_underlying(id) < entries_.size());
    return entries_[to_underlying(id)].parent;
}

auto PathPool::name(PathId id) const -> StringId
{
    assert(to_underlying(id) < entries_.size());
    return entries_[to_underlying(id)].name;
}

auto PathPool::to_string(PathId id, StringPool& pool) const -> StringId
{
    if (is_root(id)) {
        return StringId::Empty;
    }

    auto const& entry = entries_[to_underlying(id)];
    if (is_root(entry.parent)) {
        return entry.name;
    }

    auto stack = Vec<StringId> {};
    auto cur = id;
    while (!is_root(cur)) {
        stack.push_back(entries_[to_underlying(cur)].name);
        cur = entries_[to_underlying(cur)].parent;
    }

    auto buf = Buf {};
    for (auto i = stack.size(); i > 0; --i) {
        if (buf.size() > 0) {
            buf += '/';
        }
        buf.append(pool.get(stack[i - 1]));
    }

    return buf.intern(pool);
}

auto PathPool::size() const -> std::size_t
{
    return entries_.size() - 1;
}

auto PathPool::clear() -> void
{
    entries_.clear();
    children_.clear();

    entries_.push_back(Entry {});
    children_.resize(1);
}

} // namespace pup
