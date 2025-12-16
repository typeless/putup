// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/index/entry.hpp"
#include "pup/core/hash.hpp"

#include <algorithm>
#include <functional>
#include <ranges>

namespace pup::index {

auto FileEntry::to_raw(std::uint32_t name_offset) const -> RawFileEntry
{
    auto raw = RawFileEntry {};
    raw.id = id;
    raw.parent_id = parent_id;
    raw.src_id = src_id;
    raw.type = static_cast<std::uint8_t>(type);
    set_node_flags(raw, flags);
    raw.size = size;
    raw.name_offset = name_offset;
    raw.name_length = static_cast<std::uint32_t>(name.size());
    raw.content_hash = content_hash;
    return raw;
}

auto FileEntry::from_raw(RawFileEntry const& raw, std::string_view name_str) -> FileEntry
{
    return FileEntry {
        .id = raw.id,
        .parent_id = raw.parent_id,
        .src_id = raw.src_id,
        .type = static_cast<NodeType>(raw.type),
        .flags = get_node_flags(raw),
        .name = std::string { name_str },
        .path = {}, // Computed later from parent chain
        .size = raw.size,
        .content_hash = raw.content_hash,
    };
}

auto CommandEntry::to_raw(
    std::uint32_t cmd_offset,
    std::uint32_t display_offset,
    std::uint32_t env_offset) const -> RawCommandEntry
{
    auto raw = RawCommandEntry {};
    raw.id = id;
    raw.dir_id = dir_id;
    raw.cmd_offset = cmd_offset;
    raw.cmd_length = static_cast<std::uint32_t>(command.size());
    raw.display_offset = display_offset;
    raw.display_length = static_cast<std::uint32_t>(display.size());
    raw.env_offset = env_offset;
    raw.env_length = static_cast<std::uint32_t>(env.size());
    raw.flags = flags;
    return raw;
}

auto CommandEntry::from_raw(
    RawCommandEntry const& raw,
    std::string_view cmd_str,
    std::string_view display_str,
    std::string_view env_str) -> CommandEntry
{
    return CommandEntry {
        .id = raw.id,
        .dir_id = raw.dir_id,
        .command = std::string { cmd_str },
        .display = std::string { display_str },
        .env = std::string { env_str },
        .flags = raw.flags,
    };
}

auto EdgeEntry::to_raw() const -> RawEdge
{
    return RawEdge {
        .from_id = from,
        .to_id = to,
        .type = static_cast<std::uint8_t>(type),
        .reserved1 = 0,
        .reserved2 = 0,
        .group_cmd_id = static_cast<std::uint32_t>(group_cmd_id),
    };
}

auto EdgeEntry::from_raw(RawEdge const& raw) -> EdgeEntry
{
    return EdgeEntry {
        .from = raw.from_id,
        .to = raw.to_id,
        .type = static_cast<LinkType>(raw.type),
        .group_cmd_id = raw.group_cmd_id,
    };
}

auto Index::add_edge(EdgeEntry entry) -> void
{
    auto const idx = edges_.size();
    edges_.push_back(entry);
    edges_from_index_[edges_[idx].from].push_back(idx);
    edges_to_index_[edges_[idx].to].push_back(idx);
}

auto Index::find_file(std::string_view path) const -> FileEntry const*
{
    auto it = std::find_if(files_.begin(), files_.end(),
        [path](auto const& f) { return f.path == path; });
    return it != files_.end() ? &*it : nullptr;
}

auto Index::find_file_by_id(NodeId id) const -> FileEntry const*
{
    auto it = std::find_if(files_.begin(), files_.end(),
        [id](auto const& f) { return f.id == id; });
    return it != files_.end() ? &*it : nullptr;
}

auto Index::find_command_by_id(NodeId id) const -> CommandEntry const*
{
    auto it = std::find_if(commands_.begin(), commands_.end(),
        [id](auto const& c) { return c.id == id; });
    return it != commands_.end() ? &*it : nullptr;
}

auto Index::edges_from(NodeId id) const -> std::vector<EdgeEntry const*>
{
    auto result = std::vector<EdgeEntry const*> {};
    auto it = edges_from_index_.find(id);
    if (it != edges_from_index_.end()) {
        result.reserve(it->second.size());
        for (auto idx : it->second) {
            result.push_back(&edges_[idx]);
        }
    }
    return result;
}

auto Index::edges_to(NodeId id) const -> std::vector<EdgeEntry const*>
{
    auto result = std::vector<EdgeEntry const*> {};
    auto it = edges_to_index_.find(id);
    if (it != edges_to_index_.end()) {
        result.reserve(it->second.size());
        for (auto idx : it->second) {
            result.push_back(&edges_[idx]);
        }
    }
    return result;
}

auto Index::build_edge_indices() -> void
{
    edges_from_index_.clear();
    edges_to_index_.clear();

    for (auto i = std::size_t { 0 }; i < edges_.size(); ++i) {
        edges_from_index_[edges_[i].from].push_back(i);
        edges_to_index_[edges_[i].to].push_back(i);
    }
}

auto Index::compute_paths() -> void
{
    // Build id -> file index for parent lookup
    auto id_to_file = std::unordered_map<NodeId, FileEntry*> {};
    for (auto& file : files_) {
        id_to_file[file.id] = &file;
    }

    // Compute path for each file by walking parent chain
    auto path_cache = std::unordered_map<NodeId, std::string> {};

    std::function<std::string(NodeId)> get_path = [&](NodeId id) -> std::string {
        if (id == 0) {
            return "";
        }

        if (auto it = path_cache.find(id); it != path_cache.end()) {
            return it->second;
        }

        auto file_it = id_to_file.find(id);
        if (file_it == id_to_file.end()) {
            return "";
        }

        auto* file = file_it->second;
        auto path = std::string {};

        if (file->parent_id != 0) {
            auto parent_path = get_path(file->parent_id);
            if (!parent_path.empty()) {
                // Avoid double slash when parent is "/" (virtual root for external files)
                if (parent_path.back() == '/') {
                    path = parent_path + file->name;
                } else {
                    path = parent_path + "/" + file->name;
                }
            } else {
                path = file->name;
            }
        } else {
            path = file->name;
        }

        path_cache[id] = path;
        return path;
    };

    for (auto& file : files_) {
        file.path = get_path(file.id);
    }
}

auto Index::clear() -> void
{
    files_.clear();
    commands_.clear();
    edges_.clear();
    edges_from_index_.clear();
    edges_to_index_.clear();
    children_index_.clear();
}

auto Index::build_children_index() -> void
{
    children_index_.clear();

    for (auto const& file : files_) {
        children_index_[file.parent_id].push_back(file.id);
    }
}

auto Index::get_children(NodeId parent_id) const -> std::vector<NodeId>
{
    auto it = children_index_.find(parent_id);
    if (it != children_index_.end()) {
        return it->second;
    }
    return {};
}

auto Index::compute_merkle_hashes() -> void
{
    // Build children index if not already done
    if (children_index_.empty() && !files_.empty()) {
        build_children_index();
    }

    // Build id->file pointer map for O(1) lookups
    auto id_to_file = std::unordered_map<NodeId, FileEntry*> {};
    for (auto& file : files_) {
        id_to_file[file.id] = &file;
    }

    // Compute depth for each directory
    auto depths = std::unordered_map<NodeId, std::size_t> {};
    std::function<std::size_t(NodeId)> get_depth = [&](NodeId id) -> std::size_t {
        if (id == 0) {
            return 0;
        }
        if (auto it = depths.find(id); it != depths.end()) {
            return it->second;
        }

        auto file_it = id_to_file.find(id);
        if (file_it == id_to_file.end()) {
            return 0;
        }

        auto depth = std::size_t { 1 } + get_depth(file_it->second->parent_id);
        depths[id] = depth;
        return depth;
    };

    // Collect directories and compute their depths
    auto directories = std::vector<FileEntry*> {};
    for (auto& file : files_) {
        if (file.type == NodeType::Directory || file.type == NodeType::GeneratedDir) {
            get_depth(file.id);
            directories.push_back(&file);
        }
    }

    // Sort by depth descending (deepest first for bottom-up processing)
    std::sort(directories.begin(), directories.end(),
        [&depths](auto const* a, auto const* b) { return depths[a->id] > depths[b->id]; });

    // Compute Merkle hash for each directory bottom-up
    for (auto* dir : directories) {
        auto children_entries = std::vector<std::tuple<std::string_view, NodeType, Hash256 const*>> {};

        for (auto child_id : get_children(dir->id)) {
            auto child_it = id_to_file.find(child_id);
            if (child_it == id_to_file.end()) {
                continue;
            }

            auto const* child = child_it->second;
            children_entries.emplace_back(child->name, child->type, &child->content_hash);
        }

        dir->content_hash = compute_merkle_hash(children_entries);
    }
}

auto Index::has_merkle_hashes() const -> bool
{
    return std::ranges::any_of(files_, [](auto const& file) {
        return (file.type == NodeType::Directory || file.type == NodeType::GeneratedDir)
            && file.content_hash != Hash256 {};
    });
}

} // namespace pup::index
