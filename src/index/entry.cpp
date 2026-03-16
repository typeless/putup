// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/index/entry.hpp"
#include "pup/core/hash.hpp"
#include "pup/core/path_utils.hpp"

#include <charconv>
#include <functional>
#include <unordered_set>

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
) -> FileEntry
{
    return FileEntry {
        .id = static_cast<NodeId>(array_index + 1),
        .parent_id = raw.parent_id,
        .src_id = raw.src_id,
        .type = static_cast<NodeType>(raw.type),
        .flags = get_node_flags(raw),
        .name = std::string { name_str },
        .path = {},
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
    return raw;
}

auto CommandEntry::from_raw(
    RawCommandEntry const& raw,
    std::string_view instruction_pattern,
    std::string_view display_str,
    std::string_view env_str,
    std::vector<NodeId> inputs,
    std::vector<NodeId> outputs,
    std::size_t array_index
) -> CommandEntry
{
    return CommandEntry {
        .id = node_id::make_command(array_index + 1),
        .dir_id = raw.dir_id,
        .instruction_pattern = std::string { instruction_pattern },
        .display = std::string { display_str },
        .env = std::string { env_str },
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
        .group_cmd_id = group_cmd_id,
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

auto Index::find_command_by_command(std::string const& cmd) const -> CommandEntry const*
{
    auto it = command_index_.find(cmd);
    if (it != command_index_.end()) {
        return &commands_[it->second];
    }
    return nullptr;
}

auto Index::lookup_edges(
    std::unordered_map<NodeId, std::vector<std::size_t>> const& index,
    NodeId id
) const -> std::vector<EdgeEntry const*>
{
    auto result = std::vector<EdgeEntry const*> {};
    auto it = index.find(id);
    if (it != index.end()) {
        result.reserve(it->second.size());
        for (auto idx : it->second) {
            result.push_back(&edges_[idx]);
        }
    }
    return result;
}

auto Index::edges_from(NodeId id) const -> std::vector<EdgeEntry const*>
{
    return lookup_edges(edges_from_index_, id);
}

auto Index::edges_to(NodeId id) const -> std::vector<EdgeEntry const*>
{
    return lookup_edges(edges_to_index_, id);
}

auto Index::build_edge_indices() -> void
{
    edges_from_index_.clear();
    edges_to_index_.clear();

    for (auto i = std::size_t { 0 }; i < edges_.size(); ++i) {
        edges_from_index_[edges_[i].from].push_back(i);
        edges_to_index_[edges_[i].to].push_back(i);
    }

    // Rebuild command index using reconstructed command strings
    rebuild_command_index();
}

auto Index::rebuild_command_index() -> void
{
    command_index_.clear();

    for (auto i = std::size_t { 0 }; i < commands_.size(); ++i) {
        auto cmd_str = get_command_string(*this, commands_[i]);
        if (!cmd_str.empty()) {
            command_index_[std::move(cmd_str)] = i;
        }
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
    auto visiting = std::unordered_set<NodeId> {}; // Cycle detection

    std::function<std::string(NodeId)> get_path = [&](NodeId id) -> std::string {
        if (id == 0) {
            return "";
        }

        if (auto it = path_cache.find(id); it != path_cache.end()) {
            return it->second;
        }

        // Cycle detection: if we're already visiting this node, there's a cycle
        if (visiting.contains(id)) {
            return ""; // Break the cycle
        }

        auto file_it = id_to_file.find(id);
        if (file_it == id_to_file.end()) {
            return "";
        }

        auto* file = file_it->second;
        auto path = std::string {};

        visiting.insert(id);

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

        visiting.erase(id);
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

auto get_command_string(Index const& index, CommandEntry const& cmd) -> std::string
{
    if (cmd.instruction_pattern.empty()) {
        return {};
    }

    // Get directory path for relativization (if command has a source dir)
    auto source_dir = std::string_view {};
    if (cmd.dir_id != 0) {
        auto const* dir = index.find_file_by_id(cmd.dir_id);
        if (dir && !dir->path.empty()) {
            source_dir = dir->path;
        }
    }

    auto source_to_root = pup::compute_source_to_root(source_dir);

    auto get_relative_path = [&source_dir, &source_to_root](std::string_view path) {
        return pup::make_source_relative(path, source_to_root, source_dir);
    };

    auto result = std::string {};
    auto const& tmpl = cmd.instruction_pattern;
    auto pos = std::size_t { 0 };

    while (pos < tmpl.size()) {
        auto percent = tmpl.find('%', pos);

        if (percent == std::string::npos) {
            result += tmpl.substr(pos);
            break;
        }

        result += tmpl.substr(pos, percent - pos);

        if (percent + 1 >= tmpl.size()) {
            result += '%';
            pos = percent + 1;
            continue;
        }

        auto flag = tmpl[percent + 1];
        pos = percent + 2;

        if (flag == '%') {
            result += '%';
            continue;
        }

        // Check for %Nf or %No patterns (N-th input/output)
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
                        result += get_relative_path(file->path);
                    }
                }
                pos = end + 1;
                continue;
            }

            if (end < tmpl.size() && tmpl[end] == 'o') {
                if (num > 0 && static_cast<std::size_t>(num) <= cmd.outputs.size()) {
                    auto const* file = index.find_file_by_id(cmd.outputs[static_cast<std::size_t>(num - 1)]);
                    if (file) {
                        result += get_relative_path(file->path);
                    }
                }
                pos = end + 1;
                continue;
            }

            result += '%';
            pos = percent + 1;
            continue;
        }

        // Standard pattern flags
        switch (flag) {
        case 'f':
        case 'i': {
            for (std::size_t i = 0; i < cmd.inputs.size(); ++i) {
                if (i > 0) {
                    result += ' ';
                }
                auto const* file = index.find_file_by_id(cmd.inputs[i]);
                if (file) {
                    result += get_relative_path(file->path);
                }
            }
            break;
        }
        case 'b': {
            if (!cmd.inputs.empty()) {
                auto const* file = index.find_file_by_id(cmd.inputs[0]);
                if (file) {
                    result += get_basename(file->path);
                }
            }
            break;
        }
        case 'B': {
            if (!cmd.inputs.empty()) {
                auto const* file = index.find_file_by_id(cmd.inputs[0]);
                if (file) {
                    result += get_stem(file->name);
                }
            }
            break;
        }
        case 'e': {
            if (!cmd.inputs.empty()) {
                auto const* file = index.find_file_by_id(cmd.inputs[0]);
                if (file) {
                    result += get_extension(file->name);
                }
            }
            break;
        }
        case 'o': {
            if (!cmd.outputs.empty()) {
                auto const* file = index.find_file_by_id(cmd.outputs[0]);
                if (file) {
                    result += get_relative_path(file->path);
                }
            }
            break;
        }
        case 'O': {
            if (!cmd.outputs.empty()) {
                auto const* file = index.find_file_by_id(cmd.outputs[0]);
                if (file) {
                    result += get_basename(file->path);
                }
            }
            break;
        }
        case 'd': {
            // %d expands to the basename of the source directory (where Tupfile is)
            // Must match graph::expand_instruction() exactly
            if (!source_dir.empty()) {
                auto slash = source_dir.rfind('/');
                if (slash != std::string_view::npos) {
                    result += source_dir.substr(slash + 1);
                } else {
                    result += source_dir;
                }
            }
            break;
        }
        default:
            result += '%';
            result += flag;
            break;
        }
    }

    return result;
}

auto build_command_lookup(Index const& index)
    -> std::unordered_map<std::string, CommandEntry const*>
{
    auto lookup = std::unordered_map<std::string, CommandEntry const*> {};
    lookup.reserve(index.commands().size());

    for (auto const& cmd : index.commands()) {
        auto full_cmd = get_command_string(index, cmd);
        if (!full_cmd.empty()) {
            lookup.emplace(std::move(full_cmd), &cmd);
        }
    }

    return lookup;
}

} // namespace pup::index
