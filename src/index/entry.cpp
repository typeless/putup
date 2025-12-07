// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/index/entry.hpp"

#include <algorithm>

namespace pup::index {

auto FileEntry::to_raw(std::uint32_t path_offset) const -> RawFileEntry
{
    auto raw = RawFileEntry {};
    raw.id = id;
    raw.parent_id = parent_id;
    raw.src_id = src_id;
    raw.type = static_cast<std::uint8_t>(type);
    set_node_flags(raw, flags);
    raw.size = size;
    set_mtime(raw, mtime);
    raw.path_offset = path_offset;
    raw.path_length = static_cast<std::uint32_t>(path.size());
    raw.content_hash = content_hash;
    return raw;
}

auto FileEntry::from_raw(RawFileEntry const& raw, std::string_view path_str) -> FileEntry
{
    return FileEntry {
        .id = raw.id,
        .parent_id = raw.parent_id,
        .src_id = raw.src_id,
        .type = static_cast<NodeType>(raw.type),
        .flags = get_node_flags(raw),
        .path = std::string { path_str },
        .size = raw.size,
        .mtime = get_mtime(raw),
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
    for (auto const& edge : edges_) {
        if (edge.from == id)
            result.push_back(&edge);
    }
    return result;
}

auto Index::edges_to(NodeId id) const -> std::vector<EdgeEntry const*>
{
    auto result = std::vector<EdgeEntry const*> {};
    for (auto const& edge : edges_) {
        if (edge.to == id)
            result.push_back(&edge);
    }
    return result;
}

auto Index::clear() -> void
{
    files_.clear();
    commands_.clear();
    edges_.clear();
}

} // namespace pup::index
