// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/index/entry.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/node_id_map.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/index/format.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace pup::index {

auto FileEntry::to_raw(std::uint32_t name_offset) const -> RawFileEntry
{
    auto raw = RawFileEntry {};
    raw.parent_id = parent_id;
    raw.src_id = src_id;
    raw.name_offset = name_offset;
    raw.size = size;
    raw.mtime_ns = mtime_ns;
    raw.type = static_cast<std::uint8_t>(type);
    set_node_flags(raw, flags);
    raw.content_hash = content_hash;
    return raw;
}

auto FileEntry::from_raw(
    RawFileEntry const& raw,
    std::string_view name_str,
    std::size_t array_index
) -> Result<FileEntry>
{
    if (!names_node_type(raw.type)) {
        return make_error<FileEntry>(ErrorCode::IndexDamaged, "Recorded entry type names no node type");
    }
    return FileEntry {
        .id = static_cast<NodeId>(array_index + 1),
        .parent_id = raw.parent_id,
        .src_id = raw.src_id,
        .type = static_cast<NodeType>(raw.type),
        .flags = get_node_flags(raw),
        .name = global_pool().intern(name_str),
        .path = StringId::Empty,
        .size = raw.size,
        .mtime_ns = raw.mtime_ns,
        .content_hash = raw.content_hash,
    };
}

auto CommandEntry::to_raw(
    std::uint32_t instruction_offset,
    std::uint32_t display_offset,
    std::uint32_t env_offset
) const -> RawCommandEntry
{
    auto raw = RawCommandEntry {};
    raw.dir_id = dir_id;
    raw.cmd_offset = instruction_offset;
    raw.display_offset = display_offset;
    raw.env_offset = env_offset;
    raw.key = key;
    raw.signature = signature;
    raw.flags = must_rerun ? to_underlying(CommandFlag::MustRerun) : 0U;
    return raw;
}

auto CommandEntry::from_raw(
    RawCommandEntry const& raw,
    std::string_view instruction_pattern,
    std::string_view display_str,
    std::string_view env_str,
    Vec<NodeId> inputs,
    Vec<NodeId> outputs,
    std::size_t array_index
) -> CommandEntry
{
    return CommandEntry {
        .id = node_id::make_command(array_index + 1),
        .dir_id = raw.dir_id,
        .instruction_pattern = global_pool().intern(instruction_pattern),
        .display = global_pool().intern(display_str),
        .env = global_pool().intern(env_str),
        .key = raw.key,
        .signature = raw.signature,
        .must_rerun = (raw.flags & to_underlying(CommandFlag::MustRerun)) != 0U,
        .inputs = std::move(inputs),
        .outputs = std::move(outputs),
    };
}

auto EdgeEntry::to_raw() const -> RawEdge
{
    return RawEdge {
        .from_id = from,
        .to_id = to,
        .type = static_cast<std::uint8_t>(type),
        .reserved = {},
    };
}

auto EdgeEntry::from_raw(RawEdge const& raw) -> Result<EdgeEntry>
{
    if (!names_link_type(raw.type)) {
        return make_error<EdgeEntry>(ErrorCode::IndexDamaged, "Recorded edge type names no link type");
    }
    return EdgeEntry {
        .from = raw.from_id,
        .to = raw.to_id,
        .type = static_cast<LinkType>(raw.type),
    };
}

auto Index::add_edge(EdgeEntry entry) -> void
{
    assert(edges_.size() < UINT32_MAX);
    auto const idx = static_cast<std::uint32_t>(edges_.size());
    edges_.push_back(entry);

    auto old_from = edges_from_index_.get_slice(edges_[idx].from);
    edges_from_index_.set_slice(edges_[idx].from, edge_arena_.append_extend(old_from, idx));

    auto old_to = edges_to_index_.get_slice(edges_[idx].to);
    edges_to_index_.set_slice(edges_[idx].to, edge_arena_.append_extend(old_to, idx));
}

auto Index::find_file_by_id(NodeId id) const -> FileEntry const*
{
    if (id == 0 || node_id::is_command(id)) {
        return nullptr;
    }
    auto const idx = node_id::index(id) - 1;
    if (idx >= files_.size()) {
        return nullptr;
    }
    return files_[idx].id == id ? &files_[idx] : nullptr;
}

auto Index::find_command_by_id(NodeId id) const -> CommandEntry const*
{
    if (!node_id::is_command(id)) {
        return nullptr;
    }
    auto const idx = node_id::index(id);
    if (idx == 0) {
        return nullptr;
    }
    auto const vec_idx = idx - 1;
    if (vec_idx >= commands_.size()) {
        return nullptr;
    }
    return commands_[vec_idx].id == id ? &commands_[vec_idx] : nullptr;
}

auto Index::lookup_edges(NodeIdArenaIndex const& index, NodeId id) const
    -> Vec<EdgeEntry const*>
{
    auto s = index.get_slice(id);
    if (s.length == 0) {
        return {};
    }
    auto span = edge_arena_.slice(s);
    auto result = Vec<EdgeEntry const*> {};
    result.reserve(span.size());
    for (auto idx : span) {
        result.push_back(&edges_[idx]);
    }
    return result;
}

auto Index::edges_from(NodeId id) const -> Vec<EdgeEntry const*>
{
    return lookup_edges(edges_from_index_, id);
}

auto Index::edges_to(NodeId id) const -> Vec<EdgeEntry const*>
{
    return lookup_edges(edges_to_index_, id);
}

auto Index::build_edge_indices() -> void
{
    edge_arena_.clear();
    edges_from_index_.clear();
    edges_to_index_.clear();

    for (auto i = std::size_t { 0 }; i < edges_.size(); ++i) {
        auto const idx = static_cast<std::uint32_t>(i);
        auto old_from = edges_from_index_.get_slice(edges_[i].from);
        edges_from_index_.set_slice(edges_[i].from, edge_arena_.append_extend(old_from, idx));

        auto old_to = edges_to_index_.get_slice(edges_[i].to);
        edges_to_index_.set_slice(edges_[i].to, edge_arena_.append_extend(old_to, idx));
    }
}

auto Index::compute_paths() -> void
{
    auto const n = files_.size();
    if (n == 0) {
        return;
    }

    // File IDs are 1-based contiguous: files_[i].id == i + 1.
    // Use direct indexing instead of a hash map.
    assert(files_[0].id == 1 && "compute_paths requires 1-based contiguous IDs");
    assert(files_[n - 1].id == static_cast<NodeId>(n) && "compute_paths requires 1-based contiguous IDs");
    auto computed = Vec<bool> {};
    computed.resize(n);
    auto chain = Vec<std::size_t> {}; // reusable ancestor stack

    for (std::size_t i = 0; i < n; ++i) {
        if (computed[i]) {
            continue;
        }

        // Walk the parent chain upward, collecting unresolved ancestors
        chain.clear();
        auto idx = i;
        for (;;) {
            if (computed[idx]) {
                break;
            }
            chain.push_back(idx);

            auto parent_id = files_[idx].parent_id;
            if (parent_id == 0 || node_id::is_command(parent_id)) {
                break;
            }
            auto parent_idx = static_cast<std::size_t>(node_id::index(parent_id)) - 1;
            if (parent_idx >= n) {
                break;
            }

            // Cycle detection: check if parent_idx is already in our chain
            auto is_cycle = false;
            for (auto a : chain) {
                if (a == parent_idx) {
                    is_cycle = true;
                    break;
                }
            }
            if (is_cycle) {
                break;
            }
            idx = parent_idx;
        }

        // Resolve paths top-down (chain is bottom-up, so iterate in reverse)
        auto& pool = global_pool();
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            auto& file = files_[*it];
            if (file.parent_id == 0 || node_id::is_command(file.parent_id)) {
                file.path = file.name;
            } else {
                auto parent_idx = static_cast<std::size_t>(node_id::index(file.parent_id)) - 1;
                if (parent_idx < n && computed[parent_idx]) {
                    auto pp = pool.get(files_[parent_idx].path);
                    auto name_sv = pool.get(file.name);
                    if (pp.empty()) {
                        file.path = file.name;
                    } else {
                        auto combined = Buf {};
                        combined.reserve(pp.size() + 1 + name_sv.size());
                        combined += pp;
                        if (pp.back() != '/') {
                            combined += '/';
                        }
                        combined += name_sv;
                        file.path = combined.intern(pool);
                    }
                } else {
                    file.path = file.name;
                }
            }
            computed[*it] = true;
        }
    }
}

auto Index::clear() -> void
{
    files_.clear();
    commands_.clear();
    edges_.clear();
    edge_arena_.clear();
    edges_from_index_.clear();
    edges_to_index_.clear();
}

namespace {

auto get_basename(std::string_view path) -> std::string_view
{
    auto pos = path.rfind('/');
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

auto get_stem(std::string_view name) -> std::string_view
{
    auto pos = name.rfind('.');
    return pos == std::string_view::npos ? name : name.substr(0, pos);
}

auto get_extension(std::string_view name) -> std::string_view
{
    auto pos = name.rfind('.');
    return pos == std::string_view::npos ? std::string_view {} : name.substr(pos + 1);
}

} // namespace

auto get_command_string(Index const& index, CommandEntry const& cmd) -> StringId
{
    auto& pool = global_pool();
    auto tmpl = pool.get(cmd.instruction_pattern);
    if (tmpl.empty()) {
        return StringId::Empty;
    }

    auto source_dir = std::string_view {};
    if (cmd.dir_id != 0) {
        auto const* dir = index.find_file_by_id(cmd.dir_id);
        if (dir && !is_empty(dir->path)) {
            source_dir = pool.get(dir->path);
        }
    }

    auto source_to_root = pool.get(pup::compute_source_to_root(source_dir));

    auto get_relative_path = [&source_dir, &source_to_root, &pool](StringId path_id) {
        return pool.get(pup::make_source_relative(pool.get(path_id), source_to_root, source_dir));
    };

    auto buf = Buf {};
    auto pos = std::size_t { 0 };

    while (pos < tmpl.size()) {
        auto percent = tmpl.find('%', pos);

        if (percent == std::string_view::npos) {
            buf += tmpl.substr(pos);
            break;
        }

        buf += tmpl.substr(pos, percent - pos);

        if (percent + 1 >= tmpl.size()) {
            buf += '%';
            pos = percent + 1;
            continue;
        }

        auto flag = tmpl[percent + 1];
        pos = percent + 2;

        if (flag == '%') {
            buf += '%';
            continue;
        }

        if (flag >= '0' && flag <= '9') {
            auto end = pos;
            while (end < tmpl.size() && tmpl[end] >= '0' && tmpl[end] <= '9') {
                ++end;
            }

            auto num = 0;
            auto const* start_ptr = tmpl.data() + percent + 1;
            auto const* end_ptr = tmpl.data() + end;
            std::from_chars(start_ptr, end_ptr, num);

            if (end < tmpl.size() && tmpl[end] == 'f') {
                if (num > 0 && static_cast<std::size_t>(num) <= cmd.inputs.size()) {
                    auto const* file = index.find_file_by_id(cmd.inputs[static_cast<std::size_t>(num - 1)]);
                    if (file) {
                        buf += get_relative_path(file->path);
                    }
                }
                pos = end + 1;
                continue;
            }

            if (end < tmpl.size() && tmpl[end] == 'o') {
                if (num > 0 && static_cast<std::size_t>(num) <= cmd.outputs.size()) {
                    auto const* file = index.find_file_by_id(cmd.outputs[static_cast<std::size_t>(num - 1)]);
                    if (file) {
                        buf += get_relative_path(file->path);
                    }
                }
                pos = end + 1;
                continue;
            }

            buf += '%';
            pos = percent + 1;
            continue;
        }

        switch (flag) {
        case 'f':
        case 'i': {
            for (std::size_t i = 0; i < cmd.inputs.size(); ++i) {
                if (i > 0) {
                    buf += ' ';
                }
                auto const* file = index.find_file_by_id(cmd.inputs[i]);
                if (file) {
                    buf += get_relative_path(file->path);
                }
            }
            break;
        }
        case 'b': {
            if (!cmd.inputs.empty()) {
                auto const* file = index.find_file_by_id(cmd.inputs[0]);
                if (file) {
                    buf += get_basename(pool.get(file->path));
                }
            }
            break;
        }
        case 'B': {
            if (!cmd.inputs.empty()) {
                auto const* file = index.find_file_by_id(cmd.inputs[0]);
                if (file) {
                    buf += get_stem(pool.get(file->name));
                }
            }
            break;
        }
        case 'e': {
            if (!cmd.inputs.empty()) {
                auto const* file = index.find_file_by_id(cmd.inputs[0]);
                if (file) {
                    buf += get_extension(pool.get(file->name));
                }
            }
            break;
        }
        case 'o': {
            if (!cmd.outputs.empty()) {
                auto const* file = index.find_file_by_id(cmd.outputs[0]);
                if (file) {
                    buf += get_relative_path(file->path);
                }
            }
            break;
        }
        case 'O': {
            if (!cmd.outputs.empty()) {
                auto const* file = index.find_file_by_id(cmd.outputs[0]);
                if (file) {
                    buf += get_basename(pool.get(file->path));
                }
            }
            break;
        }
        case 'd': {
            if (!source_dir.empty()) {
                auto slash = source_dir.rfind('/');
                if (slash != std::string_view::npos) {
                    buf += source_dir.substr(slash + 1);
                } else {
                    buf += source_dir;
                }
            }
            break;
        }
        default:
            buf += '%';
            buf += flag;
            break;
        }
    }

    return buf.intern(pool);
}

auto FilesByPath::find(StringId path) const -> FileEntry const*
{
    auto const* it = std::lower_bound(
        entries.begin(), entries.end(), path, [](auto const& e, StringId key) { return handle_less(e.first, key); }
    );
    return (it != entries.end() && it->first == path) ? it->second : nullptr;
}

auto files_by_path(Index const& index) -> FilesByPath
{
    auto result = FilesByPath {};
    result.entries.reserve(index.files().size());

    for (auto const& file : index.files()) {
        if (!is_empty(file.path)) {
            result.entries.emplace_back(file.path, &file);
        }
    }

    std::sort(result.entries.begin(), result.entries.end(), [](auto const& a, auto const& b) { return handle_less(a.first, b.first); });
    return result;
}

} // namespace pup::index
