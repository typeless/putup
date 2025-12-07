// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/graph/dag.hpp"

#include <algorithm>

namespace pup::graph {

auto BuildGraph::add_node(Node node) -> Result<NodeId>
{
    auto const id = next_id_++;
    node.id = id;

    // Index by path for files
    if (!node.path.empty())
        path_index_[node.path] = id;

    // Index by command for command nodes
    if (node.type == NodeType::Command && !node.command.empty())
        command_index_[node.command] = id;

    nodes_.push_back(std::move(node));
    return id;
}

auto BuildGraph::add_edge(NodeId from, NodeId to, LinkType type) -> Result<void>
{
    if (!validate_node_id(from))
        return make_error<void>(ErrorCode::InvalidNodeId, "Invalid source node ID");
    if (!validate_node_id(to))
        return make_error<void>(ErrorCode::InvalidNodeId, "Invalid destination node ID");

    // Add edge
    edges_.push_back(Edge{
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

    return {};
}

auto BuildGraph::get_node(NodeId id) -> Node*
{
    for (auto& node : nodes_) {
        if (node.id == id)
            return &node;
    }
    return nullptr;
}

auto BuildGraph::get_node(NodeId id) const -> Node const*
{
    for (auto const& node : nodes_) {
        if (node.id == id)
            return &node;
    }
    return nullptr;
}

auto BuildGraph::find_by_path(std::string_view path) const -> std::optional<NodeId>
{
    auto it = path_index_.find(std::string{path});
    if (it != path_index_.end())
        return it->second;
    return std::nullopt;
}

auto BuildGraph::find_by_command(std::string_view cmd) const -> std::optional<NodeId>
{
    auto it = command_index_.find(std::string{cmd});
    if (it != command_index_.end())
        return it->second;
    return std::nullopt;
}

auto BuildGraph::nodes_of_type(NodeType type) const -> std::vector<NodeId>
{
    auto result = std::vector<NodeId>{};
    for (auto const& node : nodes_) {
        if (node.type == type)
            result.push_back(node.id);
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

auto BuildGraph::clear() -> void
{
    nodes_.clear();
    edges_.clear();
    path_index_.clear();
    command_index_.clear();
    next_id_ = 1;
}

auto BuildGraph::all_nodes() const -> std::vector<NodeId>
{
    auto result = std::vector<NodeId>{};
    result.reserve(nodes_.size());
    for (auto const& node : nodes_)
        result.push_back(node.id);
    return result;
}

auto BuildGraph::root_nodes() const -> std::vector<NodeId>
{
    auto result = std::vector<NodeId>{};
    for (auto const& node : nodes_) {
        if (node.inputs.empty() && node.order_only.empty())
            result.push_back(node.id);
    }
    return result;
}

auto BuildGraph::leaf_nodes() const -> std::vector<NodeId>
{
    auto result = std::vector<NodeId>{};
    for (auto const& node : nodes_) {
        if (node.outputs.empty())
            result.push_back(node.id);
    }
    return result;
}

auto BuildGraph::validate_node_id(NodeId id) const -> bool
{
    return get_node(id) != nullptr;
}

} // namespace pup::graph
