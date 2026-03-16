// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/dag.hpp"

#include "pup/core/metrics.hpp"
#include "pup/core/path_utils.hpp"

#include "pup/core/path.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>

namespace pup::graph {

auto make_graph() -> Graph
{
    auto graph = Graph {};

    // Initialize dir_name_index with pool pointer for transparent lookup
    graph.dir_name_index = std::unordered_map<DirNameKey, NodeId, DirNameKeyHash, DirNameKeyEqual>(
        0, DirNameKeyHash { &graph.strings }, DirNameKeyEqual { &graph.strings }
    );

    // Reserve BUILD_ROOT_ID (1) for the build root node.
    // All Generated/Ghost nodes will be parented under this node.
    // The build root's filesystem location is determined at build time
    // (source_root for in-tree, output_root for variant builds).
    graph.files.resize(2); // Index 0 unused, index 1 = build root
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

    if (!is_empty(node.name)) {
        graph.dir_name_index[DirNameKey { node.parent_dir, node.name }] = id;
    }

    auto const idx = node_id::index(id);
    if (idx >= graph.files.size()) {
        graph.files.resize(idx + 1);
    }
    graph.files[idx] = std::move(node);

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
    if (!validate_node_id(graph, from)) {
        return make_error<void>(ErrorCode::InvalidNodeId, "Invalid source node ID");
    }
    if (!validate_node_id(graph, to)) {
        return make_error<void>(ErrorCode::InvalidNodeId, "Invalid destination node ID");
    }

    auto old_to = graph.order_only_to_index.get_slice(to);
    graph.order_only_to_index.set_slice(to, graph.edge_arena.append_extend(old_to, from));

    auto old_deps = graph.order_only_dependents.get_slice(from);
    graph.order_only_dependents.set_slice(from, graph.edge_arena.append_extend(old_deps, to));

    return {};
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
    // Zero-allocation lookup using transparent hash/equal
    auto view = DirNameKeyView { parent_dir, name };
    auto it = graph.dir_name_index.find(view);
    if (it != graph.dir_name_index.end()) {
        return it->second;
    }
    return std::nullopt;
}

auto find_by_command(Graph const& graph, std::string_view cmd) -> std::optional<NodeId>
{
    assert(graph.command_index_built && "find_by_command() requires build_command_index() first");
    auto it = graph.command_str_index.find(cmd);
    if (it != graph.command_str_index.end()) {
        return it->second;
    }
    return std::nullopt;
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
        auto name = std::string { slash == std::string_view::npos ? remaining : remaining.substr(0, slash) };
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

auto nodes_of_type(Graph const& graph, NodeType type) -> std::vector<NodeId>
{
    auto result = std::vector<NodeId> {};
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

auto get_inputs(Graph const& graph, NodeId id) -> std::vector<NodeId>
{
    auto s = graph.edges_to_index.get_slice(id);
    if (s.length == 0)
        return {};
    auto span = graph.edge_arena.slice(s);
    auto result = std::vector<NodeId> {};
    result.reserve(span.size());
    for (auto idx : span) {
        result.push_back(graph.edges[idx].from);
    }
    return result;
}

auto get_outputs(Graph const& graph, NodeId id) -> std::vector<NodeId>
{
    auto s = graph.edges_from_index.get_slice(id);
    if (s.length == 0)
        return {};
    auto span = graph.edge_arena.slice(s);
    auto result = std::vector<NodeId> {};
    for (auto idx : span) {
        auto const& edge = graph.edges[idx];
        if (edge.type != LinkType::Sticky) {
            result.push_back(edge.to);
        }
    }
    return result;
}

auto get_sticky_outputs(Graph const& graph, NodeId id) -> std::vector<NodeId>
{
    auto s = graph.edges_from_index.get_slice(id);
    if (s.length == 0)
        return {};
    auto span = graph.edge_arena.slice(s);
    auto result = std::vector<NodeId> {};
    for (auto idx : span) {
        auto const& edge = graph.edges[idx];
        if (edge.type == LinkType::Sticky) {
            result.push_back(edge.to);
        }
    }
    return result;
}

auto get_order_only(Graph const& graph, NodeId id) -> std::vector<NodeId>
{
    auto s = graph.order_only_to_index.get_slice(id);
    if (s.length == 0)
        return {};
    auto span = graph.edge_arena.slice(s);
    return { span.begin(), span.end() };
}

auto get_order_only_dependents(Graph const& graph, NodeId id) -> std::vector<NodeId>
{
    auto s = graph.order_only_dependents.get_slice(id);
    if (s.length == 0)
        return {};
    auto span = graph.edge_arena.slice(s);
    return { span.begin(), span.end() };
}

auto node_count(Graph const& graph) -> std::size_t
{
    auto file_count = graph.files.empty() ? std::size_t { 0 } : graph.files.size() - 1;
    auto cmd_count = graph.commands.empty() ? std::size_t { 0 } : graph.commands.size() - 1;
    return file_count + cmd_count;
}

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
    // Preserve build root name string before clearing
    auto build_root_name_str = std::string { graph.strings.get(graph.files[BUILD_ROOT_ID].name) };

    graph.files.clear();
    graph.commands.clear();
    graph.conditions.clear();
    graph.phi_nodes.clear();
    graph.edges.clear();
    graph.edge_arena.clear();
    graph.edges_to_index.clear();
    graph.edges_from_index.clear();
    graph.order_only_to_index.clear();
    graph.order_only_dependents.clear();
    graph.dir_name_index.clear();
    graph.command_str_index.clear();
    graph.strings.clear();

    // Re-initialize dir_name_index with pool pointer
    graph.dir_name_index = std::unordered_map<DirNameKey, NodeId, DirNameKeyHash, DirNameKeyEqual>(
        0, DirNameKeyHash { &graph.strings }, DirNameKeyEqual { &graph.strings }
    );

    // Re-intern build root name
    auto build_root_name = graph.strings.intern(build_root_name_str);

    // Reinitialize build root node (same as make_graph)
    graph.files.resize(2);
    graph.files[1] = FileNode {
        .id = BUILD_ROOT_ID,
        .type = NodeType::Directory,
        .name = build_root_name,
        .parent_dir = SOURCE_ROOT_ID,
    };
    graph.next_file_id = 2;
    graph.next_command_id = node_id::make_command(1);
    graph.next_condition_id = node_id::make_condition(1);
    graph.next_phi_id = node_id::make_phi(1);
}

auto all_nodes(Graph const& graph) -> std::vector<NodeId>
{
    auto result = std::vector<NodeId> {};
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

auto root_nodes(Graph const& graph) -> std::vector<NodeId>
{
    auto has_inputs = [&](NodeId id) {
        return graph.edges_to_index.contains(id) || graph.order_only_to_index.contains(id);
    };

    auto result = std::vector<NodeId> {};
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

auto leaf_nodes(Graph const& graph) -> std::vector<NodeId>
{
    auto has_outputs = [&](NodeId id) {
        auto s = graph.edges_from_index.get_slice(id);
        if (s.length == 0)
            return false;
        auto span = graph.edge_arena.slice(s);
        for (auto idx : span) {
            if (graph.edges[idx].type != LinkType::Sticky) {
                return true;
            }
        }
        return false;
    };

    auto result = std::vector<NodeId> {};
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

    auto const name = graph.strings.get(node->name);
    if (name.empty()) {
        return "";
    }

    if (auto it = cache.find(id); it != cache.end()) {
        if (it->second.empty()) {
            return name;
        }
        return it->second;
    }

    cache[id] = "";

    auto path = std::string {};
    if (node->parent_dir != 0) {
        auto parent_path = get_full_path(graph, node->parent_dir, cache);
        if (!parent_path.empty()) {
            if (parent_path.back() == '/') {
                path = std::string { parent_path } + std::string { name };
            } else {
                path = std::string { parent_path } + "/" + std::string { name };
            }
        } else {
            path = std::string { name };
        }
    } else {
        path = std::string { name };
    }

    cache[id] = path;
    return cache[id];
}

auto get_full_path(Graph const& graph, NodeId id) -> std::string
{
    auto cache = PathCache {};
    return std::string { get_full_path(graph, id, cache) };
}

auto invalidate_path_cache(PathCache& cache, NodeId id) -> void
{
    cache.erase(id);
}

auto clear_path_cache(PathCache& cache) -> void
{
    cache.clear();
}

auto set_build_root_name(Graph& graph, std::string name) -> void
{
    auto name_id = graph.strings.intern(name);
    graph.files[BUILD_ROOT_ID].name = name_id;

    // Register in dir_name_index so lookups for "build" find BUILD_ROOT_ID
    // (BUILD_ROOT_ID was created with empty name, so wasn't indexed initially)
    graph.dir_name_index[DirNameKey { SOURCE_ROOT_ID, name_id }] = BUILD_ROOT_ID;
}

auto get_build_root_name(Graph const& graph) -> std::string_view
{
    return graph.strings.get(graph.files[BUILD_ROOT_ID].name);
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

auto intern_string(Graph& graph, std::string_view str) -> StringId
{
    return graph.strings.intern(str);
}

auto get_string(Graph const& graph, StringId id) -> std::string_view
{
    return graph.strings.get(id);
}

auto get_name(Graph const& graph, NodeId id) -> std::string_view
{
    auto const* node = get_file_node(graph, id);
    if (!node) {
        return {};
    }
    return graph.strings.get(node->name);
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
) -> std::string
{
    auto const* cmd = get_command_node(graph, cmd_id);
    if (!cmd) {
        return {};
    }

    auto pattern = graph.strings.get(cmd->instruction_id);
    if (pattern.empty()) {
        return {};
    }

    auto source_dir = graph.strings.get(cmd->source_dir);

    auto get_operand_name = [&](NodeId id) -> std::string_view {
        return get_name(graph, id);
    };

    auto result = std::string {};
    auto pos = std::size_t { 0 };

    while (pos < pattern.size()) {
        auto percent = pattern.find('%', pos);
        if (percent == std::string::npos) {
            result += pattern.substr(pos);
            break;
        }

        result += pattern.substr(pos, percent - pos);

        if (percent + 1 >= pattern.size()) {
            result += '%';
            pos = percent + 1;
            continue;
        }

        auto flag = pattern[percent + 1];
        pos = percent + 2;

        if (flag == '%') {
            result += '%';
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
                    result += get_operand_path(cmd->inputs[static_cast<std::size_t>(num - 1)]);
                }
                pos = end + 1;
                continue;
            }

            if (end < pattern.size() && pattern[end] == 'o') {
                if (num > 0 && static_cast<std::size_t>(num) <= cmd->outputs.size()) {
                    result += get_operand_path(cmd->outputs[static_cast<std::size_t>(num - 1)]);
                }
                pos = end + 1;
                continue;
            }

            result += '%';
            pos = percent + 1;
            continue;
        }

        switch (flag) {
        case 'f':
        case 'i': {
            for (std::size_t i = 0; i < cmd->inputs.size(); ++i) {
                if (i > 0) {
                    result += ' ';
                }
                result += get_operand_path(cmd->inputs[i]);
            }
            break;
        }
        case 'b': {
            if (!cmd->inputs.empty()) {
                result += path_basename(get_full_path(graph, cmd->inputs[0], cache));
            }
            break;
        }
        case 'B': {
            if (!cmd->inputs.empty()) {
                result += path_stem(get_operand_name(cmd->inputs[0]));
            }
            break;
        }
        case 'e': {
            if (!cmd->inputs.empty()) {
                result += path_extension(get_operand_name(cmd->inputs[0]));
            }
            break;
        }
        case 'o': {
            if (!cmd->outputs.empty()) {
                result += get_operand_path(cmd->outputs[0]);
            }
            break;
        }
        case 'O': {
            if (!cmd->outputs.empty()) {
                result += path_basename(get_full_path(graph, cmd->outputs[0], cache));
            }
            break;
        }
        case 'd': {
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

auto expand_instruction(Graph const& graph, NodeId cmd_id, PathCache& cache) -> std::string
{
    auto const* cmd = get_command_node(graph, cmd_id);
    if (!cmd) {
        return {};
    }
    auto source_dir = graph.strings.get(cmd->source_dir);
    auto source_to_root = pup::compute_source_to_root(source_dir);

    return expand_instruction_impl(graph, cmd_id, cache, [&](NodeId id) -> std::string {
        auto full = get_full_path(graph, id, cache);
        return pup::make_source_relative(full, source_to_root, source_dir);
    });
}

auto expand_instruction(
    Graph const& graph,
    NodeId cmd_id,
    PathCache& cache,
    std::string const& source_root,
    std::string const& config_root
) -> std::string
{
    auto const* cmd = get_command_node(graph, cmd_id);
    if (!cmd) {
        return {};
    }
    auto source_dir = graph.strings.get(cmd->source_dir);
    auto source_to_root = pup::compute_source_to_root(source_dir);
    auto canonical_cwd = std::string {};
    if (!source_root.empty()) {
        auto r = pup::platform::canonical(pup::path::join(source_root, std::string { source_dir }));
        if (r) {
            canonical_cwd = *r;
        }
    }

    return expand_instruction_impl(graph, cmd_id, cache, [&](NodeId id) -> std::string {
        auto full = get_full_path(graph, id, cache);
        if (!canonical_cwd.empty() && full.starts_with("..")) {
            auto joined = pup::path::join(source_root, full);
            auto abs = pup::platform::canonical(joined);
            if (abs) {
                return pup::path::relative(*abs, canonical_cwd);
            }
            return pup::path::relative(pup::path::normalize(joined), canonical_cwd);
        }
        if (!config_root.empty() && config_root != source_root
            && !pup::platform::exists(pup::path::join(source_root, full))
            && pup::platform::exists(pup::path::join(config_root, full))) {
            auto r = pup::platform::canonical(pup::path::join(config_root, full));
            if (r) {
                return pup::path::relative(*r, canonical_cwd);
            }
        }
        return pup::make_source_relative(full, source_to_root, source_dir);
    });
}

auto expand_instruction(Graph const& graph, NodeId cmd_id) -> std::string
{
    auto cache = PathCache {};
    return expand_instruction(graph, cmd_id, cache);
}

auto build_command_index(Graph& graph, PathCache& cache) -> void
{
    graph.command_str_index.clear();
    graph.command_index_built = true;
    auto& metrics = thread_metrics();
    for (auto i = std::size_t { 1 }; i < graph.commands.size(); ++i) {
        auto const& cmd = graph.commands[i];
        auto const id = node_id::make_command(i);
        if (cmd.id != id) {
            continue;
        }
        auto cmd_str = expand_instruction(graph, id, cache);
        ++metrics.command_expansions;
        if (!cmd_str.empty()) {
            graph.command_str_index[std::move(cmd_str)] = id;
        }
    }
}

auto get_display_str(Graph const& graph, NodeId id) -> std::string_view
{
    auto const* node = get_command_node(graph, id);
    if (!node) {
        return {};
    }
    return graph.strings.get(node->display);
}

auto get_source_dir(Graph const& graph, NodeId id) -> std::string_view
{
    auto const* node = get_command_node(graph, id);
    if (!node) {
        return {};
    }
    return graph.strings.get(node->source_dir);
}

auto get_instruction_pattern(Graph const& graph, NodeId id) -> std::string_view
{
    auto const* node = get_command_node(graph, id);
    if (!node) {
        return {};
    }
    return graph.strings.get(node->instruction_id);
}

// =============================================================================
// BuildGraph wrapper class implementation
// =============================================================================

BuildGraph::BuildGraph()
    : graph_(make_graph())
{
}

BuildGraph::~BuildGraph() = default;

BuildGraph::BuildGraph(BuildGraph&&) noexcept = default;

auto BuildGraph::operator=(BuildGraph&&) noexcept -> BuildGraph& = default;

} // namespace pup::graph
