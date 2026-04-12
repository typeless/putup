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
    // Reserve entries 0-2 for the three roots.
    // Entry 0 = Ungrounded (parent=self, name=Empty)
    // Entry 1 = SourceRoot (parent=self, name=Empty)
    // Entry 2 = BuildRoot  (parent=self, name=Empty)
    entries_.push_back(Entry { .parent = PathId::Ungrounded, .name = StringId::Empty });
    entries_.push_back(Entry { .parent = PathId::SourceRoot, .name = StringId::Empty });
    entries_.push_back(Entry { .parent = PathId::BuildRoot, .name = StringId::Empty });
    children_.resize(3);
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

auto PathPool::write(PathId id, Buf& buf, StringPool const& pool) const -> void
{
    if (is_root(id)) {
        return;
    }

    auto const& entry = entries_[to_underlying(id)];
    if (is_root(entry.parent)) {
        buf.append(pool.get(entry.name));
        return;
    }

    auto stack = Vec<StringId> {};
    auto cur = id;
    while (!is_root(cur)) {
        stack.push_back(entries_[to_underlying(cur)].name);
        cur = entries_[to_underlying(cur)].parent;
    }

    for (auto i = stack.size(); i > 0; --i) {
        if (i < stack.size()) {
            buf += '/';
        }
        buf.append(pool.get(stack[i - 1]));
    }
}

auto PathPool::root(PathId id) const -> PathId
{
    auto cur = id;
    while (!is_root(cur)) {
        cur = entries_[to_underlying(cur)].parent;
    }
    return cur;
}

auto PathPool::is_grounded(PathId id) const -> bool
{
    auto r = root(id);
    return r == PathId::SourceRoot || r == PathId::BuildRoot;
}

auto PathPool::ground(PathId id, PathId target_root) -> PathId
{
    assert(root(id) == PathId::Ungrounded);
    assert(target_root == PathId::SourceRoot || target_root == PathId::BuildRoot);

    if (id == PathId::Ungrounded) {
        return target_root;
    }

    auto stack = Vec<StringId> {};
    auto cur = id;
    while (!is_root(cur)) {
        stack.push_back(entries_[to_underlying(cur)].name);
        cur = entries_[to_underlying(cur)].parent;
    }

    auto result = target_root;
    for (auto i = stack.size(); i > 0; --i) {
        result = intern(result, stack[i - 1]);
    }
    return result;
}

auto PathPool::intern_path(std::string_view path, StringPool& pool, PathId start) -> PathId
{
    auto current = start;
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

auto PathPool::find_path(std::string_view path, StringPool const& pool, PathId start) const -> std::optional<PathId>
{
    auto current = start;
    auto remaining = path;

    while (!remaining.empty()) {
        auto slash = remaining.find('/');
        auto comp = (slash == std::string_view::npos) ? remaining : remaining.substr(0, slash);
        remaining = (slash == std::string_view::npos) ? std::string_view {} : remaining.substr(slash + 1);

        if (comp.empty() || comp == ".") {
            continue;
        }

        auto name = pool.find(comp);
        if (is_empty(name)) {
            return std::nullopt;
        }

        auto const parent_idx = to_underlying(current);
        auto const* found = children_[parent_idx].find(to_underlying(name));
        if (!found) {
            return std::nullopt;
        }

        current = make_path_id(*found);
    }

    return current;
}

auto PathPool::size() const -> std::size_t
{
    return entries_.size() - 3;
}

auto PathPool::clear() -> void
{
    entries_.clear();
    children_.clear();

    entries_.push_back(Entry { .parent = PathId::Ungrounded, .name = StringId::Empty });
    entries_.push_back(Entry { .parent = PathId::SourceRoot, .name = StringId::Empty });
    entries_.push_back(Entry { .parent = PathId::BuildRoot, .name = StringId::Empty });
    children_.resize(3);
}

} // namespace pup
