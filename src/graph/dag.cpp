// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/graph/dag.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <unordered_map>

namespace pup::graph {

namespace {

struct DirNameKey {
    NodeId parent_dir = 0;
    std::string name = {};

    auto operator==(DirNameKey const& other) const -> bool = default;
};

struct DirNameKeyHash {
    auto operator()(DirNameKey const& key) const noexcept -> std::size_t
    {
        auto h1 = std::hash<NodeId> {}(key.parent_dir);
        auto h2 = std::hash<std::string> {}(key.name);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

} // namespace

struct BuildGraph::Impl {
    std::deque<Node> nodes;
    std::vector<Edge> edges;
    std::unordered_map<DirNameKey, NodeId, DirNameKeyHash> dir_name_index;
    std::unordered_map<std::string, NodeId> command_index;
    std::unordered_map<NodeId, std::vector<NodeId>> order_only_dependents;
    mutable std::unordered_map<NodeId, std::string> path_cache;
    NodeId next_id = 1;

    [[nodiscard]] auto validate_node_id(NodeId id) const -> bool
    {
        if (id == 0 || id >= nodes.size()) {
            return false;
        }
        return nodes[id].id == id;
    }
};

BuildGraph::BuildGraph()
    : impl_(std::make_unique<Impl>())
{
}

BuildGraph::~BuildGraph() = default;

BuildGraph::BuildGraph(BuildGraph&&) noexcept = default;

auto BuildGraph::operator=(BuildGraph&&) noexcept -> BuildGraph& = default;

auto BuildGraph::add_node(Node node) -> Result<NodeId>
{
    auto const id = impl_->next_id++;
    node.id = id;

    if (!node.name.empty()) {
        impl_->dir_name_index[DirNameKey { node.parent_dir, node.name }] = id;
    }

    if (node.type == NodeType::Command && !node.command.empty()) {
        impl_->command_index[node.command] = id;
    }

    if (id >= impl_->nodes.size()) {
        impl_->nodes.resize(id + 1);
    }
    impl_->nodes[id] = std::move(node);

    return id;
}

auto BuildGraph::add_edge(NodeId from, NodeId to, LinkType type) -> Result<void>
{
    if (!impl_->validate_node_id(from)) {
        return make_error<void>(ErrorCode::InvalidNodeId, "Invalid source node ID");
    }
    if (!impl_->validate_node_id(to)) {
        return make_error<void>(ErrorCode::InvalidNodeId, "Invalid destination node ID");
    }

    impl_->edges.push_back(Edge {
        .from = from,
        .to = to,
        .type = type,
    });

    auto* from_node = get_node(from);
    auto* to_node = get_node(to);

    if (from_node) {
        from_node->outputs.push_back(to);
    }
    if (to_node) {
        to_node->inputs.push_back(from);
    }

    return {};
}

auto BuildGraph::add_order_only_edge(NodeId from, NodeId to) -> Result<void>
{
    if (!impl_->validate_node_id(from)) {
        return make_error<void>(ErrorCode::InvalidNodeId, "Invalid source node ID");
    }
    if (!impl_->validate_node_id(to)) {
        return make_error<void>(ErrorCode::InvalidNodeId, "Invalid destination node ID");
    }

    auto* to_node = get_node(to);
    if (to_node) {
        to_node->order_only.push_back(from);
    }

    impl_->order_only_dependents[from].push_back(to);

    return {};
}

auto BuildGraph::get_node(NodeId id) -> Node*
{
    if (id == 0 || id >= impl_->nodes.size()) {
        return nullptr;
    }
    auto& node = impl_->nodes[id];
    return node.id == id ? &node : nullptr;
}

auto BuildGraph::get_node(NodeId id) const -> Node const*
{
    if (id == 0 || id >= impl_->nodes.size()) {
        return nullptr;
    }
    auto const& node = impl_->nodes[id];
    return node.id == id ? &node : nullptr;
}

auto BuildGraph::find_by_dir_name(NodeId parent_dir, std::string_view name) const
    -> std::optional<NodeId>
{
    auto key = DirNameKey { parent_dir, std::string { name } };
    auto it = impl_->dir_name_index.find(key);
    if (it != impl_->dir_name_index.end()) {
        return it->second;
    }
    return std::nullopt;
}

auto BuildGraph::find_by_command(std::string_view cmd) const -> std::optional<NodeId>
{
    auto it = impl_->command_index.find(std::string { cmd });
    if (it != impl_->command_index.end()) {
        return it->second;
    }
    return std::nullopt;
}

auto BuildGraph::nodes_of_type(NodeType type) const -> std::vector<NodeId>
{
    auto result = std::vector<NodeId> {};
    for (auto i = NodeId { 1 }; i < impl_->nodes.size(); ++i) {
        if (impl_->nodes[i].id == i && impl_->nodes[i].type == type) {
            result.push_back(i);
        }
    }
    return result;
}

auto BuildGraph::get_inputs(NodeId id) const -> std::vector<NodeId>
{
    auto const* node = get_node(id);
    if (!node) {
        return {};
    }
    return node->inputs;
}

auto BuildGraph::get_outputs(NodeId id) const -> std::vector<NodeId>
{
    auto const* node = get_node(id);
    if (!node) {
        return {};
    }
    return node->outputs;
}

auto BuildGraph::get_order_only(NodeId id) const -> std::vector<NodeId>
{
    auto const* node = get_node(id);
    if (!node) {
        return {};
    }
    return node->order_only;
}

auto BuildGraph::get_order_only_dependents(NodeId id) const -> std::vector<NodeId>
{
    auto it = impl_->order_only_dependents.find(id);
    if (it != impl_->order_only_dependents.end()) {
        return it->second;
    }
    return {};
}

auto BuildGraph::edges() const -> std::vector<Edge> const&
{
    return impl_->edges;
}

auto BuildGraph::node_count() const -> std::size_t
{
    return impl_->nodes.empty() ? 0 : impl_->nodes.size() - 1;
}

auto BuildGraph::edge_count() const -> std::size_t
{
    return impl_->edges.size();
}

auto BuildGraph::empty() const -> bool
{
    return impl_->nodes.empty();
}

auto BuildGraph::clear() -> void
{
    impl_->nodes.clear();
    impl_->edges.clear();
    impl_->dir_name_index.clear();
    impl_->command_index.clear();
    impl_->order_only_dependents.clear();
    impl_->path_cache.clear();
    impl_->next_id = 1;
}

auto BuildGraph::all_nodes() const -> std::vector<NodeId>
{
    auto result = std::vector<NodeId> {};
    result.reserve(!impl_->nodes.empty() ? impl_->nodes.size() - 1 : 0);
    for (auto i = NodeId { 1 }; i < impl_->nodes.size(); ++i) {
        if (impl_->nodes[i].id == i) {
            result.push_back(i);
        }
    }
    return result;
}

auto BuildGraph::root_nodes() const -> std::vector<NodeId>
{
    auto result = std::vector<NodeId> {};
    for (auto i = NodeId { 1 }; i < impl_->nodes.size(); ++i) {
        auto const& node = impl_->nodes[i];
        if (node.id == i && node.inputs.empty() && node.order_only.empty()) {
            result.push_back(i);
        }
    }
    return result;
}

auto BuildGraph::leaf_nodes() const -> std::vector<NodeId>
{
    auto result = std::vector<NodeId> {};
    for (auto i = NodeId { 1 }; i < impl_->nodes.size(); ++i) {
        auto const& node = impl_->nodes[i];
        if (node.id == i && node.outputs.empty()) {
            result.push_back(i);
        }
    }
    return result;
}

auto BuildGraph::get_full_path(NodeId id) const -> std::string
{
    if (id == 0) {
        return "";
    }

    auto const* node = get_node(id);
    if (!node) {
        return "";
    }

    if (node->name.empty()) {
        return "";
    }

    if (auto it = impl_->path_cache.find(id); it != impl_->path_cache.end()) {
        if (it->second.empty()) {
            return node->name;
        }
        return it->second;
    }

    impl_->path_cache[id] = "";

    auto path = std::string {};
    if (node->parent_dir != 0) {
        auto parent_path = get_full_path(node->parent_dir);
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

    impl_->path_cache[id] = path;
    return path;
}

auto BuildGraph::invalidate_path_cache(NodeId id) -> void
{
    impl_->path_cache.erase(id);
}

auto BuildGraph::clear_path_cache() -> void
{
    impl_->path_cache.clear();
}

} // namespace pup::graph
