// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/config_commands.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/file_io.hpp"

namespace pup::cli {

namespace {

auto is_config_output(std::string_view path) -> bool
{
    return path.ends_with("tup.config");
}

} // anonymous namespace

auto find_config_commands(
    graph::BuildGraph const& graph,
    std::string_view source_root
) -> Vec<ConfigCommand>
{
    auto result = Vec<ConfigCommand> {};

    for (auto id : graph.all_nodes()) {
        if (!node_id::is_command(id)) {
            continue;
        }
        auto const* node = graph.get_command_node(id);
        if (!node) {
            continue;
        }

        for (auto output_id : graph.get_outputs(id)) {
            auto path_id = graph.get_full_path(output_id);
            auto path = global_pool().get(path_id);
            if (is_config_output(path)) {
                auto full_path_sv = global_pool().get(pup::path::join(source_root, path));
                result.push_back({
                    .cmd_id = id,
                    .output_path = path_id,
                    .exists = pup::platform::exists(full_path_sv),
                });
            }
        }
    }
    return result;
}

auto collect_command_dependencies(
    graph::BuildGraph const& graph,
    NodeIdMap32 const& commands
) -> NodeIdMap32
{
    auto result = NodeIdMap32 {};
    auto worklist = Vec<NodeId> {};

    auto try_add = [&](NodeId id) {
        if (!result.contains(id)) {
            result.set(id, 1);
            worklist.push_back(id);
        }
    };

    // Seed with initial commands — iterate all graph nodes, filter by membership
    for (auto id : graph.all_nodes()) {
        if (node_id::is_command(id) && commands.contains(id)) {
            try_add(id);
        }
    }

    while (!worklist.empty()) {
        auto cmd_id = worklist.back();
        worklist.pop_back();

        for (auto input_id : graph.get_inputs(cmd_id)) {
            if (node_id::is_command(input_id)) {
                try_add(input_id);
                continue;
            }

            for (auto producer_id : graph.get_inputs(input_id)) {
                if (node_id::is_command(producer_id)) {
                    try_add(producer_id);
                }
            }
        }

        auto add_producers = [&](NodeId file_id) {
            for (auto producer_id : graph.get_inputs(file_id)) {
                if (node_id::is_command(producer_id)) {
                    try_add(producer_id);
                }
            }
        };

        for (auto oo_id : graph.get_order_only(cmd_id)) {
            auto const* oo_node = graph.get_file_node(oo_id);
            if (!oo_node) {
                continue;
            }

            if (oo_node->type == NodeType::Group) {
                for (auto member_id : graph.get_inputs(oo_id)) {
                    add_producers(member_id);
                }
            } else {
                add_producers(oo_id);
            }
        }
    }

    return result;
}

} // namespace pup::cli
