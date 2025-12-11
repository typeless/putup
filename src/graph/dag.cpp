// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/graph/dag.hpp"

#include <algorithm>

namespace pup::graph {

auto BuildGraph::add_node(Node node) -> Result<NodeId>
{
    auto const id = next_id_++;
    node.id = id;

    // Index by (parent_dir, name) for tup-style lookup
    if (!node.name.empty())
        dir_name_index_[DirNameKey { node.parent_dir, node.name }] = id;

    // Index by command for command nodes
    if (node.type == NodeType::Command && !node.command.empty())
        command_index_[node.command] = id;

    // Use NodeId as vector index for O(1) lookup
    if (id >= nodes_.size())
        nodes_.resize(id + 1);
    nodes_[id] = std::move(node);

    // Build path_index from reconstructed path (derived index for find_by_path)
    auto path = get_full_path(id);
    if (!path.empty())
        path_index_[path] = id;

    return id;
}

auto BuildGraph::add_edge(NodeId from, NodeId to, LinkType type) -> Result<void>
{
    if (!validate_node_id(from))
        return make_error<void>(ErrorCode::InvalidNodeId, "Invalid source node ID");
    if (!validate_node_id(to))
        return make_error<void>(ErrorCode::InvalidNodeId, "Invalid destination node ID");

    // Add edge
    edges_.push_back(Edge {
        .from = from,
        .to = to,
        .type = type,
    });

    // Update node adjacency lists
    auto* from_node = get_node(from);
    auto* to_node = get_node(to);

    if (from_node)
        from_node->outputs.push_back(to);
    if (to_node)
        to_node->inputs.push_back(from);

    return {};
}

auto BuildGraph::add_order_only_edge(NodeId from, NodeId to) -> Result<void>
{
    if (!validate_node_id(from))
        return make_error<void>(ErrorCode::InvalidNodeId, "Invalid source node ID");
    if (!validate_node_id(to))
        return make_error<void>(ErrorCode::InvalidNodeId, "Invalid destination node ID");

    auto* to_node = get_node(to);
    if (to_node)
        to_node->order_only.push_back(from);

    // Track reverse mapping for O(1) get_order_only_dependents()
    order_only_dependents_[from].push_back(to);

    return {};
}

auto BuildGraph::get_node(NodeId id) -> Node*
{
    if (id == 0 || id >= nodes_.size())
        return nullptr;
    auto& node = nodes_[id];
    return node.id == id ? &node : nullptr;
}

auto BuildGraph::get_node(NodeId id) const -> Node const*
{
    if (id == 0 || id >= nodes_.size())
        return nullptr;
    auto const& node = nodes_[id];
    return node.id == id ? &node : nullptr;
}

auto BuildGraph::find_by_path(std::string_view path) const -> std::optional<NodeId>
{
    auto it = decltype(path_index_)::const_iterator { path_index_.find(std::string { path }) };
    if (it != path_index_.end())
        return it->second;
    return std::nullopt;
}

auto BuildGraph::find_by_dir_name(NodeId parent_dir, std::string_view name) const
    -> std::optional<NodeId>
{
    auto key = DirNameKey { parent_dir, std::string { name } };
    auto it = dir_name_index_.find(key);
    if (it != dir_name_index_.end())
        return it->second;
    return std::nullopt;
}

auto BuildGraph::find_by_command(std::string_view cmd) const -> std::optional<NodeId>
{
    auto it = decltype(command_index_)::const_iterator { command_index_.find(std::string { cmd }) };
    if (it != command_index_.end())
        return it->second;
    return std::nullopt;
}

auto BuildGraph::nodes_of_type(NodeType type) const -> std::vector<NodeId>
{
    auto result = std::vector<NodeId> {};
    for (auto i = NodeId { 1 }; i < nodes_.size(); ++i) {
        if (nodes_[i].id == i && nodes_[i].type == type)
            result.push_back(i);
    }
    return result;
}

auto BuildGraph::get_inputs(NodeId id) const -> std::vector<NodeId>
{
    auto const* node = get_node(id);
    if (!node)
        return {};
    return node->inputs;
}

auto BuildGraph::get_outputs(NodeId id) const -> std::vector<NodeId>
{
    auto const* node = get_node(id);
    if (!node)
        return {};
    return node->outputs;
}

auto BuildGraph::get_order_only(NodeId id) const -> std::vector<NodeId>
{
    auto const* node = get_node(id);
    if (!node)
        return {};
    return node->order_only;
}

auto BuildGraph::get_order_only_dependents(NodeId id) const -> std::vector<NodeId>
{
    auto it = order_only_dependents_.find(id);
    if (it != order_only_dependents_.end())
        return it->second;
    return {};
}

auto BuildGraph::clear() -> void
{
    nodes_.clear();
    edges_.clear();
    path_index_.clear();
    dir_name_index_.clear();
    command_index_.clear();
    order_only_dependents_.clear();
    path_cache_.clear();
    next_id_ = 1;
}

auto BuildGraph::all_nodes() const -> std::vector<NodeId>
{
    auto result = std::vector<NodeId> {};
    result.reserve(!nodes_.empty() ? nodes_.size() - 1 : 0);
    for (auto i = NodeId { 1 }; i < nodes_.size(); ++i) {
        if (nodes_[i].id == i)
            result.push_back(i);
    }
    return result;
}

auto BuildGraph::root_nodes() const -> std::vector<NodeId>
{
    auto result = std::vector<NodeId> {};
    for (auto i = NodeId { 1 }; i < nodes_.size(); ++i) {
        auto const& node = nodes_[i];
        if (node.id == i && node.inputs.empty() && node.order_only.empty())
            result.push_back(i);
    }
    return result;
}

auto BuildGraph::leaf_nodes() const -> std::vector<NodeId>
{
    auto result = std::vector<NodeId> {};
    for (auto i = NodeId { 1 }; i < nodes_.size(); ++i) {
        auto const& node = nodes_[i];
        if (node.id == i && node.outputs.empty())
            result.push_back(i);
    }
    return result;
}

auto BuildGraph::validate_node_id(NodeId id) const -> bool
{
    if (id == 0 || id >= nodes_.size())
        return false;
    return nodes_[id].id == id;
}

auto BuildGraph::get_full_path(NodeId id) const -> std::string
{
    if (id == 0)
        return "";

    auto const* node = get_node(id);
    if (!node)
        return "";

    // Nodes without name have no path
    if (node->name.empty())
        return "";

    // Check cache first
    if (auto it = path_cache_.find(id); it != path_cache_.end()) {
        // Empty string is sentinel for "currently visiting" - cycle detected
        if (it->second.empty())
            return node->name;
        return it->second;
    }

    // Mark as being visited (empty string sentinel) to detect cycles
    path_cache_[id] = "";

    // Reconstruct path by walking parent chain
    auto path = std::string {};
    if (node->parent_dir != 0) {
        auto parent_path = get_full_path(node->parent_dir);
        if (!parent_path.empty())
            path = parent_path + "/" + node->name;
        else
            path = node->name;
    } else {
        path = node->name;
    }

    // Cache and return
    path_cache_[id] = path;
    return path;
}

auto BuildGraph::invalidate_path_cache(NodeId id) -> void
{
    path_cache_.erase(id);
}

auto BuildGraph::clear_path_cache() -> void
{
    path_cache_.clear();
}

} // namespace pup::graph
