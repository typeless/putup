// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/dag.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/hash.hpp"
#include "pup/core/path_utils.hpp"

#include "pup/core/path.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/platform/file_io.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace pup::graph {

// Forward declarations for internal node accessors (no longer in public header)
auto get_file_node(Graph const& graph, NodeId id) -> FileNode const*;
auto get_file_node(Graph& graph, NodeId id) -> FileNode*;
auto get_command_node(Graph const& graph, NodeId id) -> CommandNode const*;
auto get_command_node(Graph& graph, NodeId id) -> CommandNode*;
auto get_condition_node(Graph const& graph, NodeId id) -> ConditionNode const*;
auto get_condition_node(Graph& graph, NodeId id) -> ConditionNode*;

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
        .path_id = PathId::BuildRoot,
    };
    graph.path_to_node.insert(to_underlying(PathId::SourceRoot), SOURCE_ROOT_ID);
    graph.path_to_node.insert(to_underlying(PathId::BuildRoot), BUILD_ROOT_ID);
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

    // Populate path_id from parent's path_id + this node's name.
    // Nodes under BUILD_ROOT_ID get BuildRoot-grounded PathIds.
    // Nodes under SOURCE_ROOT_ID (0) get SourceRoot-grounded PathIds.
    if (!is_empty(node.name)) {
        auto parent_path = PathId::SourceRoot;
        if (node.parent_dir != 0) {
            auto const* parent = get_file_node(std::as_const(graph), node.parent_dir);
            if (parent) {
                parent_path = parent->path_id;
            }
        }
        node.path_id = graph.paths.intern(parent_path, node.name);
        graph.path_to_node.insert(to_underlying(node.path_id), id);
    }

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

auto ensure_file_node(Graph& graph, PathId path_id, NodeType type) -> Result<NodeId>
{
    // Recursion terminates at a root sentinel.
    // BuildRoot/Ungrounded → BUILD_ROOT_ID, SourceRoot → SOURCE_ROOT_ID.
    if (is_root(path_id)) {
        return path_id == PathId::SourceRoot ? SOURCE_ROOT_ID : BUILD_ROOT_ID;
    }

    auto const* existing = graph.path_to_node.find(to_underlying(path_id));
    if (existing) {
        if (type == NodeType::Generated) {
            auto* node = get_file_node(graph, *existing);
            if (node && (node->type == NodeType::Ghost || node->type == NodeType::File)) {
                node->type = NodeType::Generated;
            }
        }
        return *existing;
    }

    // In-tree builds share one physical tree: a BuildRoot-grounded path may
    // already have a SourceRoot node (consumer parsed before producer sees
    // the on-disk output as a source file). Alias instead of splitting.
    if (graph.paths.root(path_id) == PathId::BuildRoot && get_build_root_name(graph).empty()) {
        auto& pool = global_pool();
        auto rel = pool.get(graph.paths.to_string(path_id, pool));
        if (auto src_pid = graph.paths.find_path(rel, pool, PathId::SourceRoot)) {
            if (auto const* hit = graph.path_to_node.find(to_underlying(*src_pid))) {
                auto* node = get_file_node(graph, *hit);
                if (node && (node->type == NodeType::Ghost || node->type == NodeType::File)) {
                    graph.path_to_node.insert(to_underlying(path_id), *hit);
                    if (type == NodeType::Generated) {
                        node->type = NodeType::Generated;
                    }
                    return *hit;
                }
            }
        }
    }

    // Lazy resolution: ungrounded PathIds are grounded before creation.
    // Try BuildRoot first (outputs are more common), then SourceRoot.
    if (!graph.paths.is_grounded(path_id)) {
        auto build_id = graph.paths.ground(path_id, PathId::BuildRoot);
        if (auto const* hit = graph.path_to_node.find(to_underlying(build_id))) {
            graph.path_to_node.insert(to_underlying(path_id), *hit);
            if (type == NodeType::Generated) {
                auto* node = get_file_node(graph, *hit);
                if (node && (node->type == NodeType::Ghost || node->type == NodeType::File)) {
                    node->type = NodeType::Generated;
                }
            }
            return *hit;
        }

        auto source_id = graph.paths.ground(path_id, PathId::SourceRoot);
        if (auto const* hit = graph.path_to_node.find(to_underlying(source_id))) {
            graph.path_to_node.insert(to_underlying(path_id), *hit);
            if (type == NodeType::Generated) {
                auto* node = get_file_node(graph, *hit);
                if (node && (node->type == NodeType::Ghost || node->type == NodeType::File)) {
                    node->type = NodeType::Generated;
                }
            }
            return *hit;
        }

        // No existing node — ground based on type and create.
        auto root = (type == NodeType::File || type == NodeType::Directory)
            ? PathId::SourceRoot
            : PathId::BuildRoot;
        return ensure_file_node(graph, graph.paths.ground(path_id, root), type);
    }

    auto parent_path = graph.paths.parent(path_id);
    auto parent_result = ensure_file_node(graph, parent_path, NodeType::Directory);
    if (!parent_result) {
        return parent_result;
    }

    auto name = graph.paths.name(path_id);
    if (auto found = find_by_dir_name(graph, *parent_result, global_pool().get(name))) {
        graph.path_to_node.insert(to_underlying(path_id), *found);
        if (type == NodeType::Generated) {
            auto* node = get_file_node(graph, *found);
            if (node && (node->type == NodeType::Ghost || node->type == NodeType::File)) {
                node->type = NodeType::Generated;
            }
        }
        return *found;
    }

    auto node = FileNode {
        .type = type,
        .name = name,
        .parent_dir = *parent_result,
    };
    return add_file_node(graph, std::move(node));
}

auto add_command_node(Graph& graph, CommandNode node) -> Result<NodeId>
{
    auto const id = graph.next_command_id;
    graph.next_command_id = node_id::make_command(node_id::index(graph.next_command_id) + 1);
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

auto get_parent_dir(Graph const& graph, NodeId id) -> NodeId
{
    auto const* node = get_file_node(graph, id);
    return node ? node->parent_dir : NodeId { 0 };
}

template<>
auto view<Inputs>(Graph const& graph, NodeId id) -> Vec<NodeId> const&
{
    static auto const empty = Vec<NodeId> {};
    auto const* node = get_command_node(graph, id);
    return node ? node->inputs : empty;
}

template<>
auto view<Outputs>(Graph const& graph, NodeId id) -> Vec<NodeId> const&
{
    static auto const empty = Vec<NodeId> {};
    auto const* node = get_command_node(graph, id);
    return node ? node->outputs : empty;
}

template<>
auto view<ExportedVars>(Graph const& graph, NodeId id) -> SortedIdVec const&
{
    static auto const empty = SortedIdVec {};
    auto const* node = get_command_node(graph, id);
    return node ? node->exported_vars : empty;
}

auto get_parent_command(Graph const& graph, NodeId id) -> NodeId
{
    auto const* node = get_command_node(graph, id);
    return node ? node->parent_command : INVALID_NODE_ID;
}

auto is_stdout_capture(Graph const& graph, NodeId id) -> bool
{
    auto const* node = get_command_node(graph, id);
    return node && node->generated_output && node->generated_output->type == GeneratedOutput::Type::Stdout;
}

namespace {
template<typename T>
auto read_file_field(Graph const& g, NodeId id, T FileNode::*field, T def) -> T
{
    auto const* node = get_file_node(g, id);
    return node ? node->*field : def;
}

template<typename T>
auto read_command_field(Graph const& g, NodeId id, T CommandNode::*field, T def) -> T
{
    auto const* node = get_command_node(g, id);
    return node ? node->*field : def;
}
} // namespace

template<>
auto get<NodeType>(Graph const& graph, NodeId id) -> NodeType
{
    if (node_id::is_command(id)) {
        return NodeType::Command;
    }
    return read_file_field(graph, id, &FileNode::type, NodeType::File);
}

template<>
auto get<NodeFlags>(Graph const& graph, NodeId id) -> NodeFlags
{
    return read_file_field(graph, id, &FileNode::flags, NodeFlags::None);
}

template<>
auto get<Hash256>(Graph const& graph, NodeId id) -> Hash256
{
    return read_file_field(graph, id, &FileNode::content_hash, Hash256 {});
}

template<>
auto get<OutputAction>(Graph const& graph, NodeId id) -> OutputAction
{
    return read_command_field(graph, id, &CommandNode::output_action, OutputAction::Normal);
}

template<>
auto get<Name>(Graph const& graph, NodeId id) -> StringId
{
    return read_file_field(graph, id, &FileNode::name, StringId::Empty);
}

template<>
auto get<Display>(Graph const& graph, NodeId id) -> StringId
{
    return read_command_field(graph, id, &CommandNode::display, StringId::Empty);
}

template<>
auto get<SourceDir>(Graph const& graph, NodeId id) -> StringId
{
    return read_command_field(graph, id, &CommandNode::source_dir, StringId::Empty);
}

template<>
auto get<InstructionPattern>(Graph const& graph, NodeId id) -> StringId
{
    auto const* cmd = get_command_node(graph, id);
    return cmd ? render_instruction(cmd->instruction) : StringId::Empty;
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

auto get_condition_node(Graph& graph, NodeId id) -> ConditionNode*
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) - Scott Meyers const_cast pattern
    return const_cast<ConditionNode*>(get_condition_node(std::as_const(graph), id));
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

auto get_phi_node(Graph& graph, NodeId id) -> PhiNode*
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) - Scott Meyers const_cast pattern
    return const_cast<PhiNode*>(get_phi_node(std::as_const(graph), id));
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

auto is_guard_satisfied(Graph const& graph, NodeId id) -> bool
{
    auto const* cmd = get_command_node(graph, id);
    if (!cmd) {
        return true;
    }
    for (auto const& guard : cmd->guards) {
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

/// Resolve a file path (possibly with build-root prefix) to a NodeId.
/// Strips the build-root prefix before looking up under BuildRoot, then
/// falls back to SourceRoot. Returns nullopt if the path isn't in the graph.
auto resolve_file_path(
    Graph const& graph,
    std::string_view path,
    StringPool const& pool
) -> std::optional<NodeId>
{
    if (path.empty()) {
        return std::nullopt;
    }

    auto build_lookup = path;
    auto build_root_name = get_build_root_name(graph);
    if (!build_root_name.empty()) {
        auto stripped = pool.get(pup::strip_path_prefix(path, build_root_name));
        if (stripped != path) {
            build_lookup = stripped;
        }
    }

    if (auto pid = graph.paths.find_path(build_lookup, pool, PathId::BuildRoot)) {
        if (auto const* hit = graph.path_to_node.find(to_underlying(*pid))) {
            return NodeId { *hit };
        }
    }
    if (auto pid = graph.paths.find_path(path, pool, PathId::SourceRoot)) {
        if (auto const* hit = graph.path_to_node.find(to_underlying(*pid))) {
            return NodeId { *hit };
        }
    }
    return std::nullopt;
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
    auto result = Vec<NodeId> {};
    edges_for_each(graph, id, dir, mask, [&](NodeId n) { result.push_back(n); });
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

/// Materialize a PathId to its display string. For BuildRoot-grounded paths,
/// prepends the build root name (e.g., "build/gcc/foo.o").
auto materialize_path(Graph const& graph, PathId path_id) -> StringId
{
    auto& pool = global_pool();
    auto path_sv = pool.get(graph.paths.to_string(path_id, pool));
    auto build_root_name = get_build_root_name(graph);
    if (!build_root_name.empty() && graph.paths.root(path_id) == PathId::BuildRoot) {
        return pup::path::join(build_root_name, path_sv);
    }
    return pool.intern(path_sv);
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

    if (is_root(node->path_id)) {
        return "";
    }

    if (cache.ids.contains(id)) {
        auto sid = make_string_id(cache.ids.get(id));
        return is_empty(sid) ? global_pool().get(node->name) : cache.pool.get(sid);
    }

    auto materialized = materialize_path(graph, node->path_id);
    auto cached_id = cache.pool.intern(global_pool().get(materialized));
    cache.ids.set(id, to_underlying(cached_id));
    return cache.pool.get(cached_id);
}

auto get_full_path(Graph const& graph, NodeId id) -> StringId
{
    if (id == 0 || node_id::is_command(id)) {
        return StringId::Empty;
    }
    auto const* node = get_file_node(graph, id);
    if (!node || is_root(node->path_id)) {
        return StringId::Empty;
    }
    return materialize_path(graph, node->path_id);
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
    // path_id stays PathId::BuildRoot (set in make_graph).
    // The name is for display only (get_full_path), not for PathId identity.
}

auto get_build_root_name(Graph const& graph) -> std::string_view
{
    return global_pool().get(graph.files[BUILD_ROOT_ID].name);
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
    return pup::path::bare_extension(name);
}

} // namespace

/// Core expansion logic parameterized on the path resolver.
template<typename PathResolver>
struct ExecSite {
    Graph const& graph;
    CommandNode const* cmd;
    PathCache& cache;
    std::string_view source_dir;
    PathResolver const& get_operand_path;

    auto operand_name(NodeId id) const -> std::string_view
    {
        return global_pool().get(get<Name>(graph, id));
    }

    auto operand_path(NodeId id) const -> std::string_view
    {
        auto const* node = get_file_node(graph, id);
        if (!node || node->type != NodeType::Group) {
            return get_operand_path(id);
        }
        auto const dir = get_operand_path(node->parent_dir);
        if (dir.empty()) {
            return operand_name(id);
        }
        auto spelled = Buf {};
        spelled += dir;
        spelled += '/';
        spelled += operand_name(id);
        return global_pool().get(spelled.intern(global_pool()));
    }

    auto append_literal(Buf& buf, StringId text) const -> void { buf += global_pool().get(text); }

    auto append_group_ref(Buf& buf, StringId name) const -> void
    {
        buf += "%<";
        buf += global_pool().get(name);
        buf += '>';
    }

    auto append_all_inputs(Buf& buf) const -> void
    {
        for (std::size_t i = 0; i < cmd->inputs.size(); ++i) {
            if (i > 0) {
                buf += ' ';
            }
            buf += operand_path(cmd->inputs[i]);
        }
    }

    auto append_input_base(Buf& buf) const -> void
    {
        if (!cmd->inputs.empty()) {
            buf += path_basename(operand_path(cmd->inputs[0]));
        }
    }

    auto append_input_noext(Buf& buf) const -> void
    {
        if (!cmd->inputs.empty()) {
            buf += path_stem(operand_name(cmd->inputs[0]));
        }
    }

    auto append_input_ext(Buf& buf) const -> void
    {
        if (!cmd->inputs.empty()) {
            buf += path_extension(operand_name(cmd->inputs[0]));
        }
    }

    auto append_all_outputs(Buf& buf) const -> void
    {
        for (std::size_t i = 0; i < cmd->outputs.size(); ++i) {
            if (i > 0) {
                buf += ' ';
            }
            buf += get_operand_path(cmd->outputs[i]);
        }
    }

    auto append_output_noext(Buf& buf) const -> void
    {
        if (cmd->outputs.size() == 1) {
            auto const only = get_operand_path(cmd->outputs[0]);
            buf += only.substr(0, only.size() - pup::path::extension(only).size());
        }
    }

    auto append_input_dir(Buf& buf) const -> void
    {
        if (source_dir.empty()) {
            return;
        }
        auto const slash = source_dir.rfind('/');
        buf += slash != std::string_view::npos ? source_dir.substr(slash + 1) : source_dir;
    }

    auto append_nth_input(Buf& buf, std::size_t index) const -> void
    {
        if (index < cmd->inputs.size()) {
            buf += operand_path(cmd->inputs[index]);
        }
    }

    auto append_nth_input_base(Buf& buf, std::size_t index) const -> void
    {
        if (index < cmd->inputs.size()) {
            buf += pup::path::filename(operand_path(cmd->inputs[index]));
        }
    }

    auto append_nth_input_noext(Buf& buf, std::size_t index) const -> void
    {
        if (index < cmd->inputs.size()) {
            auto const base = pup::path::filename(operand_path(cmd->inputs[index]));
            buf += base.substr(0, base.size() - pup::path::extension(base).size());
        }
    }

    auto append_nth_output(Buf& buf, std::size_t index) const -> void
    {
        if (index < cmd->outputs.size()) {
            buf += get_operand_path(cmd->outputs[index]);
        }
    }
};

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

    if (cmd->instruction.empty()) {
        return StringId::Empty;
    }

    return fold_instruction(
        cmd->instruction,
        ExecSite<PathResolver> {
            graph,
            cmd,
            cache,
            global_pool().get(cmd->source_dir),
            get_operand_path,
        }
    );
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

auto command_label(Graph const& graph, NodeId cmd_id, PathCache& cache) -> StringId
{
    auto display = get<Display>(graph, cmd_id);
    return is_empty(display) ? expand_instruction(graph, cmd_id, cache) : display;
}

auto command_label(Graph const& graph, NodeId cmd_id, PathCache& cache, std::string_view source_root, std::string_view config_root) -> StringId
{
    auto display = get<Display>(graph, cmd_id);
    return is_empty(display) ? expand_instruction(graph, cmd_id, cache, source_root, config_root) : display;
}

auto command_label(Graph const& graph, NodeId cmd_id) -> StringId
{
    auto display = get<Display>(graph, cmd_id);
    return is_empty(display) ? expand_instruction(graph, cmd_id) : display;
}

auto compute_command_key(Graph const& graph, NodeId cmd_id, PathCache& cache) -> Hash256
{
    auto& pool = global_pool();
    auto state = sha256_init();
    auto constexpr SEP = std::byte { 0 };

    state = sha256_update(state, pool.get(expand_instruction(graph, cmd_id, cache)));

    // Command text is Tupfile-relative, so the same rule in sibling directories renders
    // identically; without the directory those distinct rules share one key.
    state = sha256_update(state, std::span<std::byte const> { &SEP, 1 });
    state = sha256_update(state, pool.get(get<SourceDir>(graph, cmd_id)));

    // A dep-scan command is output-less and drops its parent's -o, so two compiles of one
    // source with equal flags render byte-identical scans; whose deps they inject is the
    // only thing that tells them apart. One level: a parent is rule-authored, so has none.
    if (auto parent = get_parent_command(graph, cmd_id); parent != INVALID_NODE_ID) {
        auto parent_key = compute_command_key(graph, parent, cache);
        state = sha256_update(state, std::span<std::byte const> { &SEP, 1 });
        state = sha256_update(state, std::span<std::byte const> { parent_key.data(), parent_key.size() });
    }

    return sha256_finalize(state);
}

auto compute_command_signature(Graph const& graph, NodeId cmd_id, PathCache& cache) -> Hash256
{
    auto& pool = global_pool();
    auto state = sha256_init();
    auto constexpr SEP = std::byte { 0 };

    // Base: the fully-expanded command text (instruction + operand paths + in-text vars).
    state = sha256_update(state, pool.get(expand_instruction(graph, cmd_id, cache)));

    state = sha256_update(state, std::span<std::byte const> { &SEP, 1 });
    state = sha256_update(state, pool.get(get<SourceDir>(graph, cmd_id)));

    // An output the text never names -- every extra output, a %o-less primary -- would otherwise
    // leave identity unchanged when the declaration changes; sorted for insertion independence.
    // A dep-scan command's data-flow edge to its parent shares this walk and names no file.
    auto output_paths = Vec<std::string_view> {};
    for (auto out_id : get_outputs(graph, cmd_id)) {
        auto path = get_full_path(graph, out_id, cache);
        if (path.empty()) {
            continue;
        }
        output_paths.push_back(path);
    }
    std::sort(output_paths.begin(), output_paths.end());
    for (auto path : output_paths) {
        state = sha256_update(state, std::span<std::byte const> { &SEP, 1 });
        state = sha256_update(state, path);
    }

    // Fold in (name, value-hash) of each Variable node reached via a Sticky edge.
    // This captures vars that affect output without appearing in the rendered text —
    // exported env vars the subprocess reads as $VAR, config vars gating an export, etc.
    // Sorted and deduped by name so identity is independent of edge insertion order and
    // tolerant of duplicate sticky edges (the graph permits them).
    auto vars = Vec<std::pair<std::string_view, Hash256>> {};
    for (auto var_id : edges_where(graph, cmd_id, EdgeDirection::Backward, edge_mask::sticky)) {
        if (get<NodeType>(graph, var_id) != NodeType::Variable) {
            continue;
        }
        vars.emplace_back(pool.get(get<Name>(graph, var_id)), get<Hash256>(graph, var_id));
    }
    std::sort(vars.begin(), vars.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    vars.erase(
        std::unique(vars.begin(), vars.end(), [](auto const& a, auto const& b) { return a.first == b.first; }),
        vars.end()
    );

    for (auto const& [name, value_hash] : vars) {
        state = sha256_update(state, std::span<std::byte const> { &SEP, 1 });
        state = sha256_update(state, name);
        state = sha256_update(state, std::span<std::byte const> { value_hash.data(), value_hash.size() });
    }

    return sha256_finalize(state);
}

// =============================================================================
// BuildGraph free functions
// =============================================================================

auto make_build_graph() -> BuildGraph
{
    return BuildGraph { .graph = make_graph(), .path_cache = {} };
}

auto set_build_root_name(BuildGraph& state, std::string_view name) -> void
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

        edges_for_each(graph, id, EdgeDirection::Backward, edge_mask::inputs, [&](NodeId input_id) {
            stack.push_back(input_id);
        });

        edges_for_each(graph, id, EdgeDirection::Backward, edge_mask::order_only, [&](NodeId dep_id) {
            stack.push_back(dep_id);
        });
    }

    return commands;
}

auto collect_affected_commands(
    Graph const& graph,
    Vec<StringId> const& changed_files,
    Vec<NodeId> const& forced,
    Vec<OrderingEdge> const& ordering
) -> NodeIdMap32
{
    auto& pool = global_pool();
    auto affected = NodeIdMap32 {};
    auto to_process = Vec<NodeId> {};

    auto discovered_consumers = Vec<std::pair<NodeId, NodeId>> {};
    discovered_consumers.reserve(ordering.size());
    for (auto const& edge : ordering) {
        discovered_consumers.emplace_back(edge.producer, edge.consumer);
    }
    std::sort(discovered_consumers.begin(), discovered_consumers.end());

    for (auto file_id : changed_files) {
        auto file_path = pool.get(file_id);

        auto found = resolve_file_path(graph, file_path, pool);
        if (!found) {
            continue;
        }
        auto id = *found;
        if (!affected.contains(id)) {
            affected.set(id, 1);
            to_process.push_back(id);
        }

        auto const* node = get_file_node(graph, id);
        if (node && node->type == NodeType::Generated) {
            edges_for_each(graph, id, EdgeDirection::Backward, edge_mask::inputs, [&](NodeId input_id) {
                if (!affected.contains(input_id)) {
                    affected.set(input_id, 1);
                    to_process.push_back(input_id);
                }
            });
        }
    }

    while (!to_process.empty()) {
        auto id = NodeId { to_process.back() };
        to_process.pop_back();

        edges_for_each(graph, id, EdgeDirection::Forward, edge_mask::data_flow, [&](NodeId dep_id) {
            if (!affected.contains(dep_id)) {
                affected.set(dep_id, 1);
                to_process.push_back(dep_id);
            }
        });

        edges_for_each(graph, id, EdgeDirection::Forward, edge_mask::order_only, [&](NodeId dep_id) {
            if (!affected.contains(dep_id)) {
                affected.set(dep_id, 1);
                to_process.push_back(dep_id);
            }
        });

        // A contradictory pair only adds a command here: the set only grows, so no cycle check.
        for (auto const *hop = std::lower_bound(discovered_consumers.begin(), discovered_consumers.end(), id, [](auto const& h, NodeId k) { return h.first < k; });
             hop != discovered_consumers.end() && hop->first == id;
             ++hop) {
            if (!affected.contains(hop->second)) {
                affected.set(hop->second, 1);
                to_process.push_back(hop->second);
            }
        }
    }

    // Joined after the cascade, not before: a forced command runs, but nothing about it
    // says its consumers must, and seeding it upstream would say exactly that.
    for (auto id : forced) {
        affected.set(id, 1);
    }

    // InjectImplicitDeps siblings (dep-scan commands) have no graph outputs,
    // so the cascade above can't reach them. They must run whenever their
    // parent compile runs — otherwise newly-introduced transitive includes
    // are never re-discovered, and the parent's run drops the recorded ones
    // with nothing to re-report them (#228). Walk all commands and attach any
    // whose parent_command was just marked affected.
    for (auto cmd_id : nodes_of_type(graph, NodeType::Command)) {
        auto parent = get_parent_command(graph, cmd_id);
        if (parent != INVALID_NODE_ID && affected.contains(parent)) {
            affected.set(cmd_id, 1);
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

        auto source_dir_sv = global_pool().get(get<SourceDir>(graph, id));
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
    BuildGraph const& state,
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
