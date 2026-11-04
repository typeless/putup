// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/dag.hpp"

namespace pup::test {

inline auto files_of_type(pup::graph::Graph const& graph, pup::NodeType type) -> pup::Vec<pup::graph::FileId>
{
    auto result = pup::Vec<pup::graph::FileId> {};
    for (auto id : pup::graph::file_ids(graph)) {
        if (pup::graph::get<pup::NodeType>(graph, id) == type) {
            result.push_back(id);
        }
    }
    return result;
}

inline auto commands_of(pup::graph::Graph const& graph) -> pup::Vec<pup::graph::CommandId>
{
    auto result = pup::Vec<pup::graph::CommandId> {};
    for (auto id : pup::graph::command_ids(graph)) {
        result.push_back(id);
    }
    return result;
}

} // namespace pup::test
