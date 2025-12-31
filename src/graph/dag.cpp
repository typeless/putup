// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/graph/dag.hpp"

#include <algorithm>
#include <deque>
#include <filesystem>
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
    std::deque<Node> files;    // Files, directories, groups (non-command nodes)
    std::deque<Node> commands; // Command nodes only
    std::vector<Edge> edges;
    std::unordered_map<DirNameKey, NodeId, DirNameKeyHash> dir_name_index;
    std::unordered_map<std::string, NodeId> command_str_index;
    std::unordered_map<NodeId, std::vector<NodeId>> order_only_dependents;
    mutable std::unordered_map<NodeId, std::string> path_cache;
    NodeId next_file_id = 1;
    NodeId next_command_id = make_command_id(1);

    [[nodiscard]]
    auto validate_node_id(NodeId id) const -> bool
    {
        if (id == 0) {
            return false;
        }
        if (is_command_id(id)) {
            auto idx = command_index(id);
            if (idx == 0 || idx >= commands.size()) {
                return false;
            }
            return commands[idx].id == id;
        }
        auto idx = file_index(id);
        if (idx >= files.size()) {
            return false;
        }
        return files[idx].id == id;
    }
};

BuildGraph::BuildGraph()
    : impl_(std::make_unique<Impl>())
{
    // Reserve BUILD_ROOT_ID (1) for the build root node.
    // All Generated/Ghost nodes will be parented under this node.
    // The build root's filesystem location is determined at build time
    // (source_root for in-tree, output_root for variant builds).
    impl_->files.resize(2); // Index 0 unused, index 1 = build root
    impl_->files[1] = Node {
        .id = BUILD_ROOT_ID,
        .type = NodeType::Directory,
        .name = {}, // Name set by set_build_root_name()
        .parent_dir = SOURCE_ROOT_ID,
    };
    impl_->next_file_id = 2; // Start regular nodes at ID 2
}

BuildGraph::~BuildGraph() = default;

BuildGraph::BuildGraph(BuildGraph&&) noexcept = default;

auto BuildGraph::operator=(BuildGraph&&) noexcept -> BuildGraph& = default;

auto BuildGraph::add_node(Node node) -> Result<NodeId>
{
    if (node.type == NodeType::Command) {
        auto const id = impl_->next_command_id++;
        node.id = id;

        if (!node.command.empty()) {
            impl_->command_str_index[node.command] = id;
        }

        auto const idx = command_index(id);
        if (idx >= impl_->commands.size()) {
            impl_->commands.resize(idx + 1);
        }
        impl_->commands[idx] = std::move(node);

        return id;
    }

    auto const id = impl_->next_file_id++;
    node.id = id;

    if (!node.name.empty()) {
        impl_->dir_name_index[DirNameKey { node.parent_dir, node.name }] = id;
    }

    auto const idx = file_index(id);
    if (idx >= impl_->files.size()) {
        impl_->files.resize(idx + 1);
    }
    impl_->files[idx] = std::move(node);

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

    // Direct storage access (IDs validated above)
    if (is_command_id(from)) {
        impl_->commands[command_index(from)].outputs.push_back(to);
    } else {
        impl_->files[file_index(from)].outputs.push_back(to);
    }
    if (is_command_id(to)) {
        impl_->commands[command_index(to)].inputs.push_back(from);
    } else {
        impl_->files[file_index(to)].inputs.push_back(from);
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

    // Direct storage access (ID validated above)
    if (is_command_id(to)) {
        impl_->commands[command_index(to)].order_only.push_back(from);
    } else {
        impl_->files[file_index(to)].order_only.push_back(from);
    }

    impl_->order_only_dependents[from].push_back(to);

    return {};
}

auto BuildGraph::get_node(NodeId id) -> Node*
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) - Scott Meyers const_cast pattern
    return const_cast<Node*>(std::as_const(*this).get_node(id));
}

auto BuildGraph::get_node(NodeId id) const -> Node const*
{
    if (id == 0) {
        return nullptr;
    }
    if (is_command_id(id)) {
        auto const idx = command_index(id);
        if (idx == 0 || idx >= impl_->commands.size()) {
            return nullptr;
        }
        auto const& node = impl_->commands[idx];
        return node.id == id ? &node : nullptr;
    }
    auto const idx = file_index(id);
    if (idx >= impl_->files.size()) {
        return nullptr;
    }
    auto const& node = impl_->files[idx];
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
    auto it = impl_->command_str_index.find(std::string { cmd });
    if (it != impl_->command_str_index.end()) {
        return it->second;
    }
    return std::nullopt;
}

auto BuildGraph::find_by_path(std::string_view path) const -> std::optional<NodeId>
{
    return find_by_path(path, SOURCE_ROOT_ID);
}

auto BuildGraph::find_by_path(std::string_view path, NodeId root) const -> std::optional<NodeId>
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

        auto found = find_by_dir_name(parent_id, name);
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

auto BuildGraph::nodes_of_type(NodeType type) const -> std::vector<NodeId>
{
    auto result = std::vector<NodeId> {};
    if (type == NodeType::Command) {
        for (auto i = std::size_t { 1 }; i < impl_->commands.size(); ++i) {
            auto const& node = impl_->commands[i];
            if (node.id == make_command_id(i)) {
                result.push_back(node.id);
            }
        }
    } else {
        for (auto i = NodeId { 1 }; i < impl_->files.size(); ++i) {
            auto const& node = impl_->files[i];
            if (node.id == i && node.type == type) {
                result.push_back(i);
            }
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
    auto file_count = impl_->files.empty() ? std::size_t { 0 } : impl_->files.size() - 1;
    auto cmd_count = impl_->commands.empty() ? std::size_t { 0 } : impl_->commands.size() - 1;
    return file_count + cmd_count;
}

auto BuildGraph::edge_count() const -> std::size_t
{
    return impl_->edges.size();
}

auto BuildGraph::empty() const -> bool
{
    return impl_->files.size() <= 1 && impl_->commands.size() <= 1;
}

auto BuildGraph::clear() -> void
{
    // Preserve build root name before clearing
    auto build_root_name = std::move(impl_->files[BUILD_ROOT_ID].name);

    impl_->files.clear();
    impl_->commands.clear();
    impl_->edges.clear();
    impl_->dir_name_index.clear();
    impl_->command_str_index.clear();
    impl_->order_only_dependents.clear();
    impl_->path_cache.clear();

    // Reinitialize build root node (same as constructor)
    impl_->files.resize(2);
    impl_->files[1] = Node {
        .id = BUILD_ROOT_ID,
        .type = NodeType::Directory,
        .name = std::move(build_root_name),
        .parent_dir = SOURCE_ROOT_ID,
    };
    impl_->next_file_id = 2;
    impl_->next_command_id = make_command_id(1);
}

auto BuildGraph::all_nodes() const -> std::vector<NodeId>
{
    auto result = std::vector<NodeId> {};
    auto file_count = impl_->files.empty() ? std::size_t { 0 } : impl_->files.size() - 1;
    auto cmd_count = impl_->commands.empty() ? std::size_t { 0 } : impl_->commands.size() - 1;
    result.reserve(file_count + cmd_count);

    for (auto i = NodeId { 1 }; i < impl_->files.size(); ++i) {
        if (impl_->files[i].id == i) {
            result.push_back(i);
        }
    }
    for (auto i = std::size_t { 1 }; i < impl_->commands.size(); ++i) {
        auto const id = make_command_id(i);
        if (impl_->commands[i].id == id) {
            result.push_back(id);
        }
    }
    return result;
}

auto BuildGraph::root_nodes() const -> std::vector<NodeId>
{
    auto result = std::vector<NodeId> {};
    for (auto i = NodeId { 1 }; i < impl_->files.size(); ++i) {
        auto const& node = impl_->files[i];
        if (node.id == i && node.inputs.empty() && node.order_only.empty()) {
            result.push_back(i);
        }
    }
    for (auto i = std::size_t { 1 }; i < impl_->commands.size(); ++i) {
        auto const id = make_command_id(i);
        auto const& node = impl_->commands[i];
        if (node.id == id && node.inputs.empty() && node.order_only.empty()) {
            result.push_back(id);
        }
    }
    return result;
}

auto BuildGraph::leaf_nodes() const -> std::vector<NodeId>
{
    auto result = std::vector<NodeId> {};
    for (auto i = NodeId { 1 }; i < impl_->files.size(); ++i) {
        auto const& node = impl_->files[i];
        if (node.id == i && node.outputs.empty()) {
            result.push_back(i);
        }
    }
    for (auto i = std::size_t { 1 }; i < impl_->commands.size(); ++i) {
        auto const id = make_command_id(i);
        auto const& node = impl_->commands[i];
        if (node.id == id && node.outputs.empty()) {
            result.push_back(id);
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

auto BuildGraph::set_build_root_name(std::string name) -> void
{
    impl_->files[BUILD_ROOT_ID].name = std::move(name);
    impl_->path_cache.clear(); // Invalidate all cached paths
}

auto BuildGraph::get_build_root_name() const -> std::string_view
{
    return impl_->files[BUILD_ROOT_ID].name;
}

auto BuildGraph::is_under_build_root(NodeId id) const -> bool
{
    if (id == 0 || id == BUILD_ROOT_ID) {
        return id == BUILD_ROOT_ID;
    }

    auto const* node = get_node(id);
    while (node && node->parent_dir != SOURCE_ROOT_ID) {
        if (node->parent_dir == BUILD_ROOT_ID) {
            return true;
        }
        node = get_node(node->parent_dir);
    }
    return false;
}

} // namespace pup::graph
