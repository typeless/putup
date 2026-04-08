// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/dag.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/metrics.hpp"
#include "pup/core/path_utils.hpp"

#include "pup/core/path.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace pup::graph {

auto make_graph() -> Graph
{
    auto graph = Graph {};

    // Reserve BUILD_ROOT_ID (1) for the build root node.
    // All Generated/Ghost nodes will be parented under this node.
    // The build root's filesystem location is determined at build time
    // (source_root for in-tree, output_root for variant builds).
    graph.files.resize(2); // Index 0 unused, index 1 = build root
    graph.dir_children.resize(2);
    graph.files[1] = FileNode {
        .id = BUILD_ROOT_ID,
        .type = NodeType::Directory,
        .name = StringId::Empty, // Name set by set_build_root_name()
        .parent_dir = SOURCE_ROOT_ID,
    };
    graph.next_file_id = 2; // Start regular nodes at ID 2

    return graph;
}

auto validate_node_id(Graph const& graph, NodeId id) -> bool
{
    if (id == 0) {
        return false;
    }
    if (node_id::is_command(id)) {
        auto idx = node_id::index(id);
        if (idx == 0 || idx >= graph.commands.size()) {
            return false;
        }
        return graph.commands[idx].id == id;
    }
    if (node_id::is_condition(id)) {
        auto idx = node_id::index(id);
        if (idx == 0 || idx >= graph.conditions.size()) {
            return false;
        }
        return graph.conditions[idx].id == id;
    }
    if (node_id::is_phi(id)) {
        auto idx = node_id::index(id);
        if (idx == 0 || idx >= graph.phi_nodes.size()) {
            return false;
        }
        return graph.phi_nodes[idx].id == id;
    }
    auto idx = node_id::index(id);
    if (idx >= graph.files.size()) {
        return false;
    }
    return graph.files[idx].id == id;
}

auto add_file_node(Graph& graph, FileNode node) -> Result<NodeId>
{
    auto const id = graph.next_file_id++;
    node.id = id;

    auto const idx = node_id::index(id);
    if (idx >= graph.files.size()) {
        graph.files.resize(idx + 1);
        graph.dir_children.resize(idx + 1);
    }
    graph.files[idx] = node;

    if (!is_empty(graph.files[idx].name)) {
        auto const parent_idx = node_id::index(graph.files[idx].parent_dir);
        graph.dir_children[parent_idx].insert(to_underlying(graph.files[idx].name), id);
    }

    return id;
}

auto add_command_node(Graph& graph, CommandNode node) -> Result<NodeId>
{
    auto const id = graph.next_command_id++;
    node.id = id;

    auto const idx = node_id::index(id);
    if (idx >= graph.commands.size()) {
        graph.commands.resize(idx + 1);
    }
    graph.commands[idx] = std::move(node);

    return id;
}

auto add_edge(Graph& graph, NodeId from, NodeId to, LinkType type) -> Result<void>
{
    if (!validate_node_id(graph, from)) {
        return make_error<void>(ErrorCode::InvalidNodeId, "Invalid source node ID");
    }
    if (!validate_node_id(graph, to)) {
        return make_error<void>(ErrorCode::InvalidNodeId, "Invalid destination node ID");
    }

    assert(graph.edges.size() < UINT32_MAX);
    auto const edge_idx = static_cast<std::uint32_t>(graph.edges.size());
    graph.edges.push_back(Edge {
        .from = from,
        .to = to,
        .type = type,
    });

    auto old_from = graph.edges_from_index.get_slice(from);
    graph.edges_from_index.set_slice(from, graph.edge_arena.append_extend(old_from, edge_idx));

    auto old_to = graph.edges_to_index.get_slice(to);
    graph.edges_to_index.set_slice(to, graph.edge_arena.append_extend(old_to, edge_idx));

    return {};
}

auto add_order_only_edge(Graph& graph, NodeId from, NodeId to) -> Result<void>
{
    return add_edge(graph, from, to, LinkType::OrderOnly);
}

auto get_file_node(Graph& graph, NodeId id) -> FileNode*
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) - Scott Meyers const_cast pattern
    return const_cast<FileNode*>(get_file_node(std::as_const(graph), id));
}

auto get_file_node(Graph const& graph, NodeId id) -> FileNode const*
{
    if (id == 0 || node_id::is_command(id)) {
        return nullptr;
    }
    auto const idx = node_id::index(id);
    if (idx >= graph.files.size()) {
        return nullptr;
    }
    auto const& node = graph.files[idx];
    return node.id == id ? &node : nullptr;
}

auto get_command_node(Graph& graph, NodeId id) -> CommandNode*
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) - Scott Meyers const_cast pattern
    return const_cast<CommandNode*>(get_command_node(std::as_const(graph), id));
}

auto get_command_node(Graph const& graph, NodeId id) -> CommandNode const*
{
    if (!node_id::is_command(id)) {
        return nullptr;
    }
    auto const idx = node_id::index(id);
    if (idx == 0 || idx >= graph.commands.size()) {
        return nullptr;
    }
    auto const& node = graph.commands[idx];
    return node.id == id ? &node : nullptr;
}

auto add_condition_node(Graph& graph, ConditionNode node) -> Result<NodeId>
{
    auto const id = graph.next_condition_id;
    graph.next_condition_id = node_id::make_condition(node_id::index(graph.next_condition_id) + 1);
    node.id = id;

    auto const idx = node_id::index(id);
    if (idx >= graph.conditions.size()) {
        graph.conditions.resize(idx + 1);
    }
    graph.conditions[idx] = std::move(node);

    return id;
}

auto get_condition_node(Graph& graph, NodeId id) -> ConditionNode*
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) - Scott Meyers const_cast pattern
    return const_cast<ConditionNode*>(get_condition_node(std::as_const(graph), id));
}

auto get_condition_node(Graph const& graph, NodeId id) -> ConditionNode const*
{
    if (!node_id::is_condition(id)) {
        return nullptr;
    }
    auto const idx = node_id::index(id);
    if (idx == 0 || idx >= graph.conditions.size()) {
        return nullptr;
    }
    auto const& node = graph.conditions[idx];
    return node.id == id ? &node : nullptr;
}

auto add_phi_node(Graph& graph, PhiNode node) -> Result<NodeId>
{
    auto const id = graph.next_phi_id;
    graph.next_phi_id = node_id::make_phi(node_id::index(graph.next_phi_id) + 1);
    node.id = id;

    auto const idx = node_id::index(id);
    if (idx >= graph.phi_nodes.size()) {
        graph.phi_nodes.resize(idx + 1);
    }
    graph.phi_nodes[idx] = std::move(node);

    return id;
}

auto get_phi_node(Graph& graph, NodeId id) -> PhiNode*
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) - Scott Meyers const_cast pattern
    return const_cast<PhiNode*>(get_phi_node(std::as_const(graph), id));
}

auto get_phi_node(Graph const& graph, NodeId id) -> PhiNode const*
{
    if (!node_id::is_phi(id)) {
        return nullptr;
    }
    auto const idx = node_id::index(id);
    if (idx == 0 || idx >= graph.phi_nodes.size()) {
        return nullptr;
    }
    auto const& node = graph.phi_nodes[idx];
    return node.id == id ? &node : nullptr;
}

auto resolve_phi_node(Graph const& graph, NodeId phi_id) -> NodeId
{
    auto const* phi = get_phi_node(graph, phi_id);
    if (!phi) {
        return INVALID_NODE_ID;
    }

    auto const* cond = get_condition_node(graph, phi->condition);
    if (!cond) {
        return INVALID_NODE_ID;
    }

    return cond->current_value ? phi->then_output : phi->else_output;
}

auto is_guard_satisfied(Graph const& graph, CommandNode const& cmd) -> bool
{
    for (auto const& guard : cmd.guards) {
        auto const* cond = get_condition_node(graph, guard.condition);
        if (!cond) {
            return false;
        }
        if (cond->current_value != guard.polarity) {
            return false;
        }
    }
    return true;
}

auto find_by_dir_name(Graph const& graph, NodeId parent_dir, std::string_view name)
    -> std::optional<NodeId>
{
    auto name_id = global_pool().find(name);
    if (is_empty(name_id)) {
        return std::nullopt;
    }
    auto const parent_idx = node_id::index(parent_dir);
    if (parent_idx >= graph.dir_children.size()) {
        return std::nullopt;
    }
    auto const* found = graph.dir_children[parent_idx].find(to_underlying(name_id));
    if (!found) {
        return std::nullopt;
    }
    return *found;
}

auto find_by_command(Graph const& graph, std::string_view cmd) -> std::optional<NodeId>
{
    assert(graph.command_index_built && "find_by_command() requires build_command_index() first");
    auto cmd_id = graph.command_strings.find(cmd);
    if (is_empty(cmd_id)) {
        return std::nullopt;
    }
    auto const* found = graph.command_index.find(to_underlying(cmd_id));
    if (!found) {
        return std::nullopt;
    }
    return *found;
}

auto find_by_path(Graph const& graph, std::string_view path) -> std::optional<NodeId>
{
    return find_by_path(graph, path, SOURCE_ROOT_ID);
}

auto find_by_path(Graph const& graph, std::string_view path, NodeId root) -> std::optional<NodeId>
{
    if (path.empty()) {
        return std::nullopt;
    }

    auto parent_id = root;
    auto remaining = path;

    while (!remaining.empty()) {
        auto slash = remaining.find('/');
        auto name = slash == std::string_view::npos ? remaining : remaining.substr(0, slash);
        remaining = (slash == std::string_view::npos) ? std::string_view {} : remaining.substr(slash + 1);
        if (name.empty() || name == ".") {
            continue;
        }

        auto found = find_by_dir_name(graph, parent_id, name);
        if (!found) {
            return std::nullopt;
        }

        parent_id = *found;
    }

    // For root=0, we need parent_id != 0 to be valid
    // For root=BUILD_ROOT_ID, any result is valid (including BUILD_ROOT_ID itself if path is empty after normalization)
    if (root == SOURCE_ROOT_ID) {
        return parent_id != SOURCE_ROOT_ID ? std::optional { parent_id } : std::nullopt;
    }
    return parent_id != root ? std::optional { parent_id } : std::nullopt;
}

auto nodes_of_type(Graph const& graph, NodeType type) -> Vec<NodeId>
{
    auto result = Vec<NodeId> {};
    if (type == NodeType::Command) {
        for (auto i = std::size_t { 1 }; i < graph.commands.size(); ++i) {
            auto const& node = graph.commands[i];
            if (node.id == node_id::make_command(i)) {
                result.push_back(node.id);
            }
        }
    } else {
        for (auto i = NodeId { 1 }; i < graph.files.size(); ++i) {
            auto const& node = graph.files[i];
            if (node.id == i && node.type == type) {
                result.push_back(i);
            }
        }
    }
    return result;
}

auto edges_where(Graph const& graph, NodeId id, EdgeDirection dir, LinkTypeMask mask) -> Vec<NodeId>
{
    auto const& index = (dir == EdgeDirection::Forward)
        ? graph.edges_from_index
        : graph.edges_to_index;

    auto s = index.get_slice(id);
    if (s.length == 0) {
        return {};
    }
    auto span = graph.edge_arena.slice(s);
    auto result = Vec<NodeId> {};
    for (auto idx : span) {
        auto const& edge = graph.edges[idx];
        if (link_type_bit(edge.type) & mask) {
            result.push_back(dir == EdgeDirection::Forward ? edge.to : edge.from);
        }
    }
    return result;
}

auto get_inputs(Graph const& graph, NodeId id) -> Vec<NodeId>
{
    return edges_where(graph, id, EdgeDirection::Backward, edge_mask::inputs);
}

auto get_outputs(Graph const& graph, NodeId id) -> Vec<NodeId>
{
    return edges_where(graph, id, EdgeDirection::Forward, edge_mask::data_flow);
}

auto get_sticky_outputs(Graph const& graph, NodeId id) -> Vec<NodeId>
{
    return edges_where(graph, id, EdgeDirection::Forward, edge_mask::sticky);
}

auto get_order_only(Graph const& graph, NodeId id) -> Vec<NodeId>
{
    return edges_where(graph, id, EdgeDirection::Backward, edge_mask::order_only);
}

auto get_order_only_dependents(Graph const& graph, NodeId id) -> Vec<NodeId>
{
    return edges_where(graph, id, EdgeDirection::Forward, edge_mask::order_only);
}

auto node_count(Graph const& graph) -> std::size_t
{
    auto file_count = graph.files.empty() ? std::size_t { 0 } : graph.files.size() - 1;
    auto cmd_count = graph.commands.empty() ? std::size_t { 0 } : graph.commands.size() - 1;
    return file_count + cmd_count;
}

// Includes all edge types: Normal, Sticky, OrderOnly.
auto edge_count(Graph const& graph) -> std::size_t
{
    return graph.edges.size();
}

auto empty(Graph const& graph) -> bool
{
    return graph.files.size() <= 1 && graph.commands.size() <= 1;
}

auto clear(Graph& graph) -> void
{
    // Preserve build root name StringId before clearing (global_pool is process-wide, not cleared)
    auto build_root_name_id = graph.files[BUILD_ROOT_ID].name;

    graph.files.clear();
    graph.commands.clear();
    graph.conditions.clear();
    graph.phi_nodes.clear();
    graph.edges.clear();
    graph.edge_arena.clear();
    graph.edges_to_index.clear();
    graph.edges_from_index.clear();
    graph.dir_children.clear();
    graph.command_strings.clear();
    graph.command_index.clear();
    graph.command_index_built = false;
    // build_root_name_id is still valid (global_pool is not cleared)
    auto build_root_name = build_root_name_id;

    // Reinitialize build root node (same as make_graph)
    graph.files.resize(2);
    graph.dir_children.resize(2);
    graph.files[1] = FileNode {
        .id = BUILD_ROOT_ID,
        .type = NodeType::Directory,
        .name = build_root_name,
        .parent_dir = SOURCE_ROOT_ID,
    };
    if (!is_empty(build_root_name)) {
        graph.dir_children[0].insert(to_underlying(build_root_name), BUILD_ROOT_ID);
    }
    graph.next_file_id = 2;
    graph.next_command_id = node_id::make_command(1);
    graph.next_condition_id = node_id::make_condition(1);
    graph.next_phi_id = node_id::make_phi(1);
}

auto all_nodes(Graph const& graph) -> Vec<NodeId>
{
    auto result = Vec<NodeId> {};
    auto file_count = graph.files.empty() ? std::size_t { 0 } : graph.files.size() - 1;
    auto cmd_count = graph.commands.empty() ? std::size_t { 0 } : graph.commands.size() - 1;
    result.reserve(file_count + cmd_count);

    for (auto i = NodeId { 1 }; i < graph.files.size(); ++i) {
        if (graph.files[i].id == i) {
            result.push_back(i);
        }
    }
    for (auto i = std::size_t { 1 }; i < graph.commands.size(); ++i) {
        auto const id = node_id::make_command(i);
        if (graph.commands[i].id == id) {
            result.push_back(id);
        }
    }
    return result;
}

auto root_nodes(Graph const& graph) -> Vec<NodeId>
{
    auto has_inputs = [&](NodeId id) {
        return graph.edges_to_index.contains(id);
    };

    auto result = Vec<NodeId> {};
    for (auto i = NodeId { 1 }; i < graph.files.size(); ++i) {
        auto const& node = graph.files[i];
        if (node.id == i && !has_inputs(i)) {
            result.push_back(i);
        }
    }
    for (auto i = std::size_t { 1 }; i < graph.commands.size(); ++i) {
        auto const id = node_id::make_command(i);
        auto const& node = graph.commands[i];
        if (node.id == id && !has_inputs(id)) {
            result.push_back(id);
        }
    }
    return result;
}

auto leaf_nodes(Graph const& graph) -> Vec<NodeId>
{
    auto has_outputs = [&](NodeId id) {
        auto s = graph.edges_from_index.get_slice(id);
        if (s.length == 0) {
            return false;
        }
        auto span = graph.edge_arena.slice(s);
        for (auto idx : span) {
            if (graph.edges[idx].type != LinkType::Sticky) {
                return true;
            }
        }
        return false;
    };

    auto result = Vec<NodeId> {};
    for (auto i = NodeId { 1 }; i < graph.files.size(); ++i) {
        auto const& node = graph.files[i];
        if (node.id == i && !has_outputs(i)) {
            result.push_back(i);
        }
    }
    for (auto i = std::size_t { 1 }; i < graph.commands.size(); ++i) {
        auto const id = node_id::make_command(i);
        auto const& node = graph.commands[i];
        if (node.id == id && !has_outputs(id)) {
            result.push_back(id);
        }
    }
    return result;
}

auto get_full_path(Graph const& graph, NodeId id, PathCache& cache) -> std::string_view
{
    if (id == 0 || node_id::is_command(id)) {
        return "";
    }

    auto const* node = get_file_node(graph, id);
    if (!node) {
        return "";
    }

    auto const name = global_pool().get(node->name);
    if (name.empty()) {
        return "";
    }

    if (cache.ids.contains(id)) {
        auto sid = make_string_id(cache.ids.get(id));
        if (is_empty(sid)) {
            return name;
        }
        return cache.pool.get(sid);
    }

    cache.ids.set(id, 0);

    auto buf = Buf {};
    if (node->parent_dir != 0) {
        auto parent_path = get_full_path(graph, node->parent_dir, cache);
        if (!parent_path.empty()) {
            buf += parent_path;
            if (parent_path.back() != '/') {
                buf += '/';
            }
            buf += name;
        } else {
            buf += name;
        }
    } else {
        buf += name;
    }

    auto path_id = cache.pool.intern(buf.view());
    cache.ids.set(id, to_underlying(path_id));
    return cache.pool.get(path_id);
}

auto get_full_path(Graph const& graph, NodeId id) -> StringId
{
    auto cache = PathCache {};
    return global_pool().intern(get_full_path(graph, id, cache));
}

auto invalidate_path_cache(PathCache& cache, NodeId id) -> void
{
    cache.ids.remove(id);
}

auto clear_path_cache(PathCache& cache) -> void
{
    cache.ids.clear();
    cache.pool.clear();
}

auto set_build_root_name(Graph& graph, std::string_view name) -> void
{
    auto old_name = graph.files[BUILD_ROOT_ID].name;
    if (!is_empty(old_name)) {
        graph.dir_children[0].remove(to_underlying(old_name));
    }

    auto name_id = global_pool().intern(name);
    graph.files[BUILD_ROOT_ID].name = name_id;

    if (!is_empty(name_id)) {
        graph.dir_children[0].insert(to_underlying(name_id), BUILD_ROOT_ID);
    }
}

auto get_build_root_name(Graph const& graph) -> std::string_view
{
    return global_pool().get(graph.files[BUILD_ROOT_ID].name);
}

auto is_under_build_root(Graph const& graph, NodeId id) -> bool
{
    if (id == 0 || id == BUILD_ROOT_ID || node_id::is_command(id)) {
        return id == BUILD_ROOT_ID;
    }

    auto const* node = get_file_node(graph, id);
    while (node && node->parent_dir != SOURCE_ROOT_ID) {
        if (node->parent_dir == BUILD_ROOT_ID) {
            return true;
        }
        node = get_file_node(graph, node->parent_dir);
    }
    return false;
}

auto get_name(Graph const& graph, NodeId id) -> std::string_view
{
    auto const* node = get_file_node(graph, id);
    if (!node) {
        return {};
    }
    return global_pool().get(node->name);
}

namespace {

auto path_basename(std::string_view path) -> std::string_view
{
    return pup::path::filename(path);
}

auto path_stem(std::string_view name) -> std::string_view
{
    return pup::path::stem(name);
}

auto path_extension(std::string_view name) -> std::string_view
{
    auto ext = pup::path::extension(name);
    if (!ext.empty() && ext[0] == '.') {
        ext.remove_prefix(1);
    }
    return ext;
}

} // namespace

/// Core expansion logic parameterized on the path resolver.
template<typename PathResolver>
auto expand_instruction_impl(
    Graph const& graph,
    NodeId cmd_id,
    PathCache& cache,
    PathResolver const& get_operand_path
) -> StringId
{
    auto const* cmd = get_command_node(graph, cmd_id);
    if (!cmd) {
        return StringId::Empty;
    }

    auto pattern = global_pool().get(cmd->instruction_id);
    if (pattern.empty()) {
        return StringId::Empty;
    }

    auto source_dir = global_pool().get(cmd->source_dir);

    auto get_operand_name = [&](NodeId id) -> std::string_view {
        return get_name(graph, id);
    };

    auto buf = Buf {};
    auto pos = std::size_t { 0 };

    while (pos < pattern.size()) {
        auto percent = pattern.find('%', pos);
        if (percent == std::string_view::npos) {
            buf += pattern.substr(pos);
            break;
        }

        buf += pattern.substr(pos, percent - pos);

        if (percent + 1 >= pattern.size()) {
            buf += '%';
            pos = percent + 1;
            continue;
        }

        auto flag = pattern[percent + 1];
        pos = percent + 2;

        if (flag == '%') {
            buf += '%';
            continue;
        }

        if (flag >= '0' && flag <= '9') {
            auto end = pos;
            while (end < pattern.size() && pattern[end] >= '0' && pattern[end] <= '9') {
                ++end;
            }

            auto num = 0;
            auto const* start_ptr = pattern.data() + percent + 1;
            auto const* end_ptr = pattern.data() + end;
            std::from_chars(start_ptr, end_ptr, num);

            if (end < pattern.size() && pattern[end] == 'f') {
                if (num > 0 && static_cast<std::size_t>(num) <= cmd->inputs.size()) {
                    buf += get_operand_path(cmd->inputs[static_cast<std::size_t>(num - 1)]);
                }
                pos = end + 1;
                continue;
            }

            if (end < pattern.size() && pattern[end] == 'o') {
                if (num > 0 && static_cast<std::size_t>(num) <= cmd->outputs.size()) {
                    buf += get_operand_path(cmd->outputs[static_cast<std::size_t>(num - 1)]);
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
            for (std::size_t i = 0; i < cmd->inputs.size(); ++i) {
                if (i > 0) {
                    buf += ' ';
                }
                buf += get_operand_path(cmd->inputs[i]);
            }
            break;
        }
        case 'b': {
            if (!cmd->inputs.empty()) {
                buf += path_basename(get_full_path(graph, cmd->inputs[0], cache));
            }
            break;
        }
        case 'B': {
            if (!cmd->inputs.empty()) {
                buf += path_stem(get_operand_name(cmd->inputs[0]));
            }
            break;
        }
        case 'e': {
            if (!cmd->inputs.empty()) {
                buf += path_extension(get_operand_name(cmd->inputs[0]));
            }
            break;
        }
        case 'o': {
            if (!cmd->outputs.empty()) {
                buf += get_operand_path(cmd->outputs[0]);
            }
            break;
        }
        case 'O': {
            if (!cmd->outputs.empty()) {
                buf += path_basename(get_full_path(graph, cmd->outputs[0], cache));
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

    return buf.intern(global_pool());
}

auto expand_instruction(Graph const& graph, NodeId cmd_id, PathCache& cache) -> StringId
{
    auto const* cmd = get_command_node(graph, cmd_id);
    if (!cmd) {
        return StringId::Empty;
    }
    auto& pool = global_pool();
    auto source_dir = pool.get(cmd->source_dir);
    auto source_to_root = pool.get(pup::compute_source_to_root(source_dir));

    return expand_instruction_impl(graph, cmd_id, cache, [&](NodeId id) -> std::string_view {
        auto full = get_full_path(graph, id, cache);
        return pool.get(pup::make_source_relative(full, source_to_root, source_dir));
    });
}

auto expand_instruction(
    Graph const& graph,
    NodeId cmd_id,
    PathCache& cache,
    std::string_view source_root,
    std::string_view config_root
) -> StringId
{
    auto const* cmd = get_command_node(graph, cmd_id);
    if (!cmd) {
        return StringId::Empty;
    }
    auto& pool = global_pool();
    auto source_dir = pool.get(cmd->source_dir);
    auto source_to_root = pool.get(pup::compute_source_to_root(source_dir));
    auto canonical_cwd_id = StringId::Empty;
    if (!source_root.empty()) {
        auto r = pup::platform::canonical(pool.get(pup::path::join(source_root, source_dir)));
        if (r) {
            canonical_cwd_id = *r;
        }
    }
    auto canonical_cwd = pool.get(canonical_cwd_id);

    return expand_instruction_impl(graph, cmd_id, cache, [&](NodeId id) -> std::string_view {
        auto full = get_full_path(graph, id, cache);
        if (!canonical_cwd.empty() && full.starts_with("..")) {
            auto joined_sv = pool.get(pup::path::join(source_root, full));
            auto abs = pup::platform::canonical(joined_sv);
            if (abs) {
                return pool.get(pup::path::relative(pool.get(*abs), canonical_cwd));
            }
            return pool.get(pup::path::relative(pool.get(pup::path::normalize(joined_sv)), canonical_cwd));
        }
        if (!config_root.empty() && config_root != source_root
            && !pup::platform::exists(pool.get(pup::path::join(source_root, full)))
            && pup::platform::exists(pool.get(pup::path::join(config_root, full)))) {
            auto r = pup::platform::canonical(pool.get(pup::path::join(config_root, full)));
            if (r) {
                return pool.get(pup::path::relative(pool.get(*r), canonical_cwd));
            }
        }
        return pool.get(pup::make_source_relative(full, source_to_root, source_dir));
    });
}

auto expand_instruction(Graph const& graph, NodeId cmd_id) -> StringId
{
    auto cache = PathCache {};
    return expand_instruction(graph, cmd_id, cache);
}

auto build_command_index(Graph& graph, PathCache& cache) -> void
{
    graph.command_strings.clear();
    graph.command_index.clear();
    graph.command_index_built = true;
    auto& metrics = thread_metrics();
    for (auto i = std::size_t { 1 }; i < graph.commands.size(); ++i) {
        auto const& cmd = graph.commands[i];
        auto const id = node_id::make_command(i);
        if (cmd.id != id) {
            continue;
        }
        auto cmd_id_str = expand_instruction(graph, id, cache);
        ++metrics.command_expansions;
        if (!is_empty(cmd_id_str)) {
            auto cmd_sv = global_pool().get(cmd_id_str);
            auto str_id = graph.command_strings.intern(cmd_sv);
            graph.command_index.insert(to_underlying(str_id), id);
        }
    }
}

auto get_display_str(Graph const& graph, NodeId id) -> std::string_view
{
    auto const* node = get_command_node(graph, id);
    if (!node) {
        return {};
    }
    return global_pool().get(node->display);
}

auto get_source_dir(Graph const& graph, NodeId id) -> std::string_view
{
    auto const* node = get_command_node(graph, id);
    if (!node) {
        return {};
    }
    return global_pool().get(node->source_dir);
}

auto get_instruction_pattern(Graph const& graph, NodeId id) -> std::string_view
{
    auto const* node = get_command_node(graph, id);
    if (!node) {
        return {};
    }
    return global_pool().get(node->instruction_id);
}

// =============================================================================
// BuildState free functions
// =============================================================================

auto make_build_state() -> BuildState
{
    return BuildState { .graph = make_graph(), .path_cache = {} };
}

auto set_build_root_name(BuildState& state, std::string_view name) -> void
{
    graph::set_build_root_name(state.graph, name);
    graph::clear_path_cache(state.path_cache);
}

// =============================================================================
// Graph algorithms (moved from scheduler — these operate on Graph, not Scheduler)
// =============================================================================

auto collect_required_commands(Graph const& graph, Vec<NodeId> const& target_ids) -> NodeIdMap32
{
    auto visited = NodeIdMap32 {};
    auto commands = NodeIdMap32 {};
    auto stack = Vec<NodeId> {};
    for (auto id : target_ids) {
        stack.push_back(id);
    }

    while (!stack.empty()) {
        auto id = stack.back();
        stack.pop_back();

        if (visited.contains(id)) {
            continue;
        }
        visited.set(id, 1);

        if (node_id::is_command(id) && get_command_node(graph, id)) {
            commands.set(id, 1);
        }

        for (auto input_id : get_inputs(graph, id)) {
            stack.push_back(input_id);
        }

        for (auto dep_id : get_order_only(graph, id)) {
            stack.push_back(dep_id);
        }
    }

    return commands;
}

auto collect_affected_commands(Graph const& graph, Vec<StringId> const& changed_files) -> NodeIdMap32
{
    auto& pool = global_pool();

    auto path_to_id = Vec<std::pair<std::string_view, NodeId>> {};
    for (auto id : all_nodes(graph)) {
        auto path_id = get_full_path(graph, id);
        if (!is_empty(path_id)) {
            path_to_id.emplace_back(pool.get(path_id), id);
        }
    }
    std::sort(path_to_id.begin(), path_to_id.end());

    auto affected = NodeIdMap32 {};
    auto to_process = Vec<NodeId> {};

    for (auto file_id : changed_files) {
        auto file_path = pool.get(file_id);
        auto it = std::lower_bound(path_to_id.begin(), path_to_id.end(), file_path, [](auto const& p, auto const& k) { return p.first < k; });
        if (it != path_to_id.end() && it->first == file_path) {
            auto id = it->second;
            if (!affected.contains(id)) {
                affected.set(id, 1);
                to_process.push_back(id);
            }

            auto const* node = get_file_node(graph, id);
            if (node && node->type == NodeType::Generated) {
                for (auto input_id : get_inputs(graph, id)) {
                    if (!affected.contains(input_id)) {
                        affected.set(input_id, 1);
                        to_process.push_back(input_id);
                    }
                }
            }
        }
    }

    while (!to_process.empty()) {
        auto id = NodeId { to_process.back() };
        to_process.pop_back();

        for (auto dep_id : get_outputs(graph, id)) {
            if (!affected.contains(dep_id)) {
                affected.set(dep_id, 1);
                to_process.push_back(dep_id);
            }
        }

        for (auto dep_id : get_order_only_dependents(graph, id)) {
            if (!affected.contains(dep_id)) {
                affected.set(dep_id, 1);
                to_process.push_back(dep_id);
            }
        }
    }

    return affected;
}

namespace {

/// Walk backward through the DAG from commands in scope, returning all
/// reachable nodes (the transitive upstream closure).
auto walk_upstream_from_scope(
    Graph const& graph,
    Vec<StringId> const& scopes
) -> Vec<NodeId>
{
    if (scopes.empty()) {
        return {};
    }

    auto visited = NodeIdMap32 {};
    auto result = Vec<NodeId> {};
    auto stack = Vec<NodeId> {};

    for (auto id : all_nodes(graph)) {
        if (!node_id::is_command(id)) {
            continue;
        }
        auto const* node = get_command_node(graph, id);
        if (!node) {
            continue;
        }

        auto source_dir_sv = get_source_dir(graph, id);
        if (!is_path_in_any_scope(source_dir_sv, scopes)) {
            continue;
        }

        visited.set(id, 1);
        result.push_back(id);

        for (auto input_id : get_inputs(graph, id)) {
            stack.push_back(input_id);
        }
        for (auto dep_id : get_order_only(graph, id)) {
            stack.push_back(dep_id);
        }
    }

    while (!stack.empty()) {
        auto id = stack.back();
        stack.pop_back();

        if (visited.contains(id)) {
            continue;
        }
        visited.set(id, 1);
        result.push_back(id);

        for (auto input_id : get_inputs(graph, id)) {
            stack.push_back(input_id);
        }
        for (auto dep_id : get_order_only(graph, id)) {
            stack.push_back(dep_id);
        }
    }

    return result;
}

} // anonymous namespace

auto collect_scope_with_upstream_commands(
    Graph const& graph,
    Vec<StringId> const& scopes
) -> NodeIdMap32
{
    auto commands = NodeIdMap32 {};
    for (auto id : walk_upstream_from_scope(graph, scopes)) {
        if (node_id::is_command(id) && get_command_node(graph, id)) {
            commands.set(id, 1);
        }
    }
    return commands;
}

auto collect_upstream_files(
    BuildState const& state,
    Vec<StringId> const& scopes
) -> Vec<std::string_view>
{
    auto const& g = state.graph;
    auto upstream = Vec<std::string_view> {};
    for (auto id : walk_upstream_from_scope(g, scopes)) {
        if (node_id::is_command(id)) {
            continue;
        }
        auto const* node = get_file_node(g, id);
        if (node && (node->type == NodeType::File || node->type == NodeType::Generated)) {
            auto path_sv = get_full_path(g, id, state.path_cache);
            if (!path_sv.empty()) {
                upstream.push_back(path_sv);
            }
        }
    }
    std::sort(upstream.begin(), upstream.end());
    upstream.erase(std::unique(upstream.begin(), upstream.end()), upstream.end());
    return upstream;
}

} // namespace pup::graph
