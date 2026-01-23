// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/dag.hpp"

#include <algorithm>
#include <filesystem>

namespace pup::graph {

auto make_graph() -> Graph
{
    auto graph = Graph {};

    // Reserve BUILD_ROOT_ID (1) for the build root node.
    // All Generated/Ghost nodes will be parented under this node.
    // The build root's filesystem location is determined at build time
    // (source_root for in-tree, output_root for variant builds).
    graph.files.resize(2); // Index 0 unused, index 1 = build root
    graph.files[1] = Node {
        .id = BUILD_ROOT_ID,
        .type = NodeType::Directory,
        .name = {}, // Name set by set_build_root_name()
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
    if (is_command_id(id)) {
        auto idx = command_index(id);
        if (idx == 0 || idx >= graph.commands.size()) {
            return false;
        }
        return graph.commands[idx].id == id;
    }
    auto idx = file_index(id);
    if (idx >= graph.files.size()) {
        return false;
    }
    return graph.files[idx].id == id;
}

auto add_node(Graph& graph, Node node) -> Result<NodeId>
{
    if (node.type == NodeType::Command) {
        auto const id = graph.next_command_id++;
        node.id = id;

        if (!node.command.empty()) {
            graph.command_str_index[node.command] = id;
        }

        auto const idx = command_index(id);
        if (idx >= graph.commands.size()) {
            graph.commands.resize(idx + 1);
        }
        graph.commands[idx] = std::move(node);

        return id;
    }

    auto const id = graph.next_file_id++;
    node.id = id;

    if (!node.name.empty()) {
        graph.dir_name_index[DirNameKey { node.parent_dir, node.name }] = id;
    }

    auto const idx = file_index(id);
    if (idx >= graph.files.size()) {
        graph.files.resize(idx + 1);
    }
    graph.files[idx] = std::move(node);

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

    graph.edges.push_back(Edge {
        .from = from,
        .to = to,
        .type = type,
    });

    // Route to appropriate output vector based on edge type
    // Sticky edges go to sticky_outputs, all others to outputs
    auto& target_vector = (type == LinkType::Sticky)
        ? (is_command_id(from) ? graph.commands[command_index(from)].sticky_outputs
                               : graph.files[file_index(from)].sticky_outputs)
        : (is_command_id(from) ? graph.commands[command_index(from)].outputs
                               : graph.files[file_index(from)].outputs);
    target_vector.push_back(to);

    // Inputs receive all edge types
    if (is_command_id(to)) {
        graph.commands[command_index(to)].inputs.push_back(from);
    } else {
        graph.files[file_index(to)].inputs.push_back(from);
    }

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

    // Direct storage access (ID validated above)
    if (is_command_id(to)) {
        graph.commands[command_index(to)].order_only.push_back(from);
    } else {
        graph.files[file_index(to)].order_only.push_back(from);
    }

    graph.order_only_dependents[from].push_back(to);

    return {};
}

auto get_node(Graph& graph, NodeId id) -> Node*
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) - Scott Meyers const_cast pattern
    return const_cast<Node*>(get_node(std::as_const(graph), id));
}

auto get_node(Graph const& graph, NodeId id) -> Node const*
{
    if (id == 0) {
        return nullptr;
    }
    if (is_command_id(id)) {
        auto const idx = command_index(id);
        if (idx == 0 || idx >= graph.commands.size()) {
            return nullptr;
        }
        auto const& node = graph.commands[idx];
        return node.id == id ? &node : nullptr;
    }
    auto const idx = file_index(id);
    if (idx >= graph.files.size()) {
        return nullptr;
    }
    auto const& node = graph.files[idx];
    return node.id == id ? &node : nullptr;
}

auto find_by_dir_name(Graph const& graph, NodeId parent_dir, std::string_view name)
    -> std::optional<NodeId>
{
    auto key = DirNameKey { parent_dir, std::string { name } };
    auto it = graph.dir_name_index.find(key);
    if (it != graph.dir_name_index.end()) {
        return it->second;
    }
    return std::nullopt;
}

auto find_by_command(Graph const& graph, std::string_view cmd) -> std::optional<NodeId>
{
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

    auto p = std::filesystem::path { path };
    auto parent_id = root;

    for (auto const& component : p) {
        auto name = component.string();
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
            if (node.id == make_command_id(i)) {
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
    auto const* node = get_node(graph, id);
    if (!node) {
        return {};
    }
    return node->inputs;
}

auto get_outputs(Graph const& graph, NodeId id) -> std::vector<NodeId>
{
    auto const* node = get_node(graph, id);
    if (!node) {
        return {};
    }
    return node->outputs;
}

auto get_sticky_outputs(Graph const& graph, NodeId id) -> std::vector<NodeId>
{
    auto const* node = get_node(graph, id);
    if (!node) {
        return {};
    }
    return node->sticky_outputs;
}

auto get_order_only(Graph const& graph, NodeId id) -> std::vector<NodeId>
{
    auto const* node = get_node(graph, id);
    if (!node) {
        return {};
    }
    return node->order_only;
}

auto get_order_only_dependents(Graph const& graph, NodeId id) -> std::vector<NodeId>
{
    auto it = graph.order_only_dependents.find(id);
    if (it != graph.order_only_dependents.end()) {
        return it->second;
    }
    return {};
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
    // Preserve build root name before clearing
    auto build_root_name = std::move(graph.files[BUILD_ROOT_ID].name);

    graph.files.clear();
    graph.commands.clear();
    graph.edges.clear();
    graph.dir_name_index.clear();
    graph.command_str_index.clear();
    graph.order_only_dependents.clear();
    graph.path_cache.clear();

    // Reinitialize build root node (same as make_graph)
    graph.files.resize(2);
    graph.files[1] = Node {
        .id = BUILD_ROOT_ID,
        .type = NodeType::Directory,
        .name = std::move(build_root_name),
        .parent_dir = SOURCE_ROOT_ID,
    };
    graph.next_file_id = 2;
    graph.next_command_id = make_command_id(1);
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
        auto const id = make_command_id(i);
        if (graph.commands[i].id == id) {
            result.push_back(id);
        }
    }
    return result;
}

auto root_nodes(Graph const& graph) -> std::vector<NodeId>
{
    auto result = std::vector<NodeId> {};
    for (auto i = NodeId { 1 }; i < graph.files.size(); ++i) {
        auto const& node = graph.files[i];
        if (node.id == i && node.inputs.empty() && node.order_only.empty()) {
            result.push_back(i);
        }
    }
    for (auto i = std::size_t { 1 }; i < graph.commands.size(); ++i) {
        auto const id = make_command_id(i);
        auto const& node = graph.commands[i];
        if (node.id == id && node.inputs.empty() && node.order_only.empty()) {
            result.push_back(id);
        }
    }
    return result;
}

auto leaf_nodes(Graph const& graph) -> std::vector<NodeId>
{
    auto result = std::vector<NodeId> {};
    for (auto i = NodeId { 1 }; i < graph.files.size(); ++i) {
        auto const& node = graph.files[i];
        if (node.id == i && node.outputs.empty()) {
            result.push_back(i);
        }
    }
    for (auto i = std::size_t { 1 }; i < graph.commands.size(); ++i) {
        auto const id = make_command_id(i);
        auto const& node = graph.commands[i];
        if (node.id == id && node.outputs.empty()) {
            result.push_back(id);
        }
    }
    return result;
}

auto get_full_path(Graph const& graph, NodeId id) -> std::string
{
    if (id == 0) {
        return "";
    }

    auto const* node = get_node(graph, id);
    if (!node) {
        return "";
    }

    if (node->name.empty()) {
        return "";
    }

    if (auto it = graph.path_cache.find(id); it != graph.path_cache.end()) {
        if (it->second.empty()) {
            return node->name;
        }
        return it->second;
    }

    graph.path_cache[id] = "";

    auto path = std::string {};
    if (node->parent_dir != 0) {
        auto parent_path = get_full_path(graph, node->parent_dir);
        if (!parent_path.empty()) {
            if (parent_path.back() == '/') {
                path = parent_path + node->name;
            } else {
                path = parent_path + "/" + node->name;
            }
        } else {
            path = node->name;
        }
    } else {
        path = node->name;
    }

    graph.path_cache[id] = path;
    return path;
}

auto invalidate_path_cache(Graph& graph, NodeId id) -> void
{
    graph.path_cache.erase(id);
}

auto clear_path_cache(Graph& graph) -> void
{
    graph.path_cache.clear();
}

auto set_build_root_name(Graph& graph, std::string name) -> void
{
    graph.files[BUILD_ROOT_ID].name = std::move(name);
    graph.path_cache.clear(); // Invalidate all cached paths
}

auto get_build_root_name(Graph const& graph) -> std::string_view
{
    return graph.files[BUILD_ROOT_ID].name;
}

auto is_under_build_root(Graph const& graph, NodeId id) -> bool
{
    if (id == 0 || id == BUILD_ROOT_ID) {
        return id == BUILD_ROOT_ID;
    }

    auto const* node = get_node(graph, id);
    while (node && node->parent_dir != SOURCE_ROOT_ID) {
        if (node->parent_dir == BUILD_ROOT_ID) {
            return true;
        }
        node = get_node(graph, node->parent_dir);
    }
    return false;
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
