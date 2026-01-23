// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/config_commands.hpp"

#include <filesystem>

namespace pup::cli {

namespace {

auto is_config_output(std::string const& path) -> bool
{
    return path.ends_with("tup.config");
}

} // anonymous namespace

auto find_config_commands(
    graph::BuildGraph const& graph,
    std::filesystem::path const& source_root
) -> std::vector<ConfigCommand>
{
    auto result = std::vector<ConfigCommand> {};

    for (auto id : graph.all_nodes()) {
        if (!is_command_id(id)) {
            continue;
        }
        auto const* node = graph.get_command_node(id);
        if (!node) {
            continue;
        }

        for (auto output_id : graph.get_outputs(id)) {
            auto path = graph.get_full_path(output_id);
            if (is_config_output(path)) {
                // Paths in graph are project-root-relative
                auto full_path = source_root / path;
                result.push_back({
                    .cmd_id = id,
                    .output_path = path,
                    .exists = std::filesystem::exists(full_path),
                });
            }
        }
    }
    return result;
}

auto collect_command_dependencies(
    graph::BuildGraph const& graph,
    std::set<NodeId> const& commands
) -> std::set<NodeId>
{
    auto result = std::set<NodeId> { commands };
    auto worklist = std::vector<NodeId> { commands.begin(), commands.end() };

    while (!worklist.empty()) {
        auto cmd_id = worklist.back();
        worklist.pop_back();

        for (auto input_id : graph.get_inputs(cmd_id)) {
            if (is_command_id(input_id)) {
                if (result.insert(input_id).second) {
                    worklist.push_back(input_id);
                }
                continue;
            }

            for (auto producer_id : graph.get_inputs(input_id)) {
                if (is_command_id(producer_id)) {
                    if (result.insert(producer_id).second) {
                        worklist.push_back(producer_id);
                    }
                }
            }
        }

        for (auto oo_id : graph.get_order_only(cmd_id)) {
            for (auto producer_id : graph.get_inputs(oo_id)) {
                if (is_command_id(producer_id)) {
                    if (result.insert(producer_id).second) {
                        worklist.push_back(producer_id);
                    }
                }
            }
        }
    }

    return result;
}

} // namespace pup::cli
