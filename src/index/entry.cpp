// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/index/entry.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/instruction.hpp"
#include "pup/core/node_id_map.hpp"
#include "pup/core/path.hpp"
#include "pup/core/path_utils.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/index/format.hpp"

#include <algorithm>
#include <cassert>
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
    auto const raw_flags = static_cast<std::uint16_t>(get_node_flags(raw));
    if ((raw_flags & ~READABLE_NODE_FLAGS_MASK) != 0) {
        return make_error<FileEntry>(ErrorCode::IndexDamaged, "Recorded entry flags carry a bit no flag names");
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
) -> Result<CommandEntry>
{
    if ((raw.flags & ~RECORDED_COMMAND_FLAGS_MASK) != 0U) {
        return make_error<CommandEntry>(ErrorCode::IndexDamaged, "Recorded command flags carry a bit no flag names");
    }
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

class IndexSite final {
public:
    IndexSite(Index const& index, CommandEntry const& cmd, std::string_view source_dir)
        : m_index { index }
        , m_cmd { cmd }
        , m_source_dir { source_dir }
        , m_source_to_root { global_pool().get(pup::compute_source_to_root(source_dir)) }
    {
    }

    auto append_literal(Buf& buf, StringId text) const -> void { buf += global_pool().get(text); }

    auto append_group_ref(Buf& buf, StringId name) const -> void
    {
        buf += "%<";
        buf += global_pool().get(name);
        buf += '>';
    }

    auto append_all_inputs(Buf& buf) const -> void { append_all(buf, m_cmd.inputs); }

    auto append_input_base(Buf& buf) const -> void
    {
        if (!m_cmd.inputs.empty()) {
            buf += pup::path::filename(operand_path(m_cmd.inputs[0]));
        }
    }

    auto append_input_noext(Buf& buf) const -> void
    {
        if (!m_cmd.inputs.empty()) {
            buf += pup::path::stem(operand_path(m_cmd.inputs[0]));
        }
    }

    auto append_input_ext(Buf& buf) const -> void
    {
        if (!m_cmd.inputs.empty()) {
            buf += pup::path::bare_extension(operand_path(m_cmd.inputs[0]));
        }
    }

    auto append_all_outputs(Buf& buf) const -> void { append_all(buf, m_cmd.outputs); }

    auto append_output_noext(Buf& buf) const -> void
    {
        if (m_cmd.outputs.size() == 1) {
            auto const only = operand_path(m_cmd.outputs[0]);
            buf += only.substr(0, only.size() - pup::path::extension(only).size());
        }
    }

    auto append_input_dir(Buf& buf) const -> void
    {
        if (m_source_dir.empty()) {
            return;
        }
        auto const slash = m_source_dir.rfind('/');
        buf += slash != std::string_view::npos ? m_source_dir.substr(slash + 1) : m_source_dir;
    }

    auto append_nth_input(Buf& buf, std::size_t index) const -> void
    {
        if (index < m_cmd.inputs.size()) {
            buf += operand_path(m_cmd.inputs[index]);
        }
    }

    auto append_nth_input_base(Buf& buf, std::size_t index) const -> void
    {
        if (index < m_cmd.inputs.size()) {
            buf += pup::path::filename(operand_path(m_cmd.inputs[index]));
        }
    }

    auto append_nth_input_noext(Buf& buf, std::size_t index) const -> void
    {
        if (index < m_cmd.inputs.size()) {
            buf += pup::path::stem(operand_path(m_cmd.inputs[index]));
        }
    }

    auto append_nth_output(Buf& buf, std::size_t index) const -> void
    {
        if (index < m_cmd.outputs.size()) {
            buf += operand_path(m_cmd.outputs[index]);
        }
    }

private:
    auto operand_path(NodeId id) const -> std::string_view
    {
        auto const* file = m_index.find_file_by_id(id);
        if (!file) {
            return {};
        }
        auto& pool = global_pool();
        return pool.get(pup::make_source_relative(pool.get(file->path), m_source_to_root, m_source_dir));
    }

    auto append_all(Buf& buf, Vec<NodeId> const& ids) const -> void
    {
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (i > 0) {
                buf += ' ';
            }
            buf += operand_path(ids[i]);
        }
    }

    Index const& m_index;
    CommandEntry const& m_cmd;
    std::string_view m_source_dir;
    std::string_view m_source_to_root;
};

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

    auto atoms = parse_instruction(tmpl);
    if (!atoms) {
        return cmd.instruction_pattern;
    }
    return fold_instruction(*atoms, IndexSite { index, cmd, source_dir });
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
