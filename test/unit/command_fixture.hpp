// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#pragma once

#include "pup/core/instruction.hpp"
#include "pup/core/result.hpp"
#include "pup/core/sorted_id_vec.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/token_list.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/dag.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace pup::test {

struct Command {
    StringId display = StringId::Empty;
    StringId source_dir = StringId::Empty;
    StringId rule_file = StringId::Empty;
    std::uint32_t rule_line = 0;
    Instruction instruction = {};
    TokenList<NodeId> inputs = {};
    TokenList<NodeId> order_only_inputs = {};
    TokenList<NodeId> outputs = {};
    SortedIdVec exported_vars = {};
    std::optional<NodeId> parent_command = {};
    Vec<graph::Guard> guards = {};
};

template<std::size_t I>
auto push_operands(graph::CommandStore::Builder<I>& part, TokenList<NodeId> const& operands) -> void
{
    for (auto token = std::uint32_t { 1 }; token <= operands.token_count(); ++token) {
        for (auto const id : operands.token(token)) {
            part.push(graph::Operand { .id = id, .token = token });
        }
    }
}

inline auto add_command(graph::Graph& graph, Command const& command) -> Result<graph::CommandId>
{
    auto const head = graph::CommandHead {
        .display = command.display,
        .source_dir = command.source_dir,
        .rule_file = command.rule_file,
        .rule_line = command.rule_line,
        .parent_command = command.parent_command.value_or(0),
        .input_tokens = command.inputs.token_count(),
        .output_tokens = command.outputs.token_count(),
        .order_only_tokens = command.order_only_inputs.token_count(),
    };
    auto [id, atoms] = graph::open_command(graph, head);
    for (auto const atom : command.instruction) {
        atoms.push(atom);
    }
    auto exported = std::move(atoms).next();
    for (auto const var : command.exported_vars) {
        exported.push(make_string_id(var));
    }
    auto guards = std::move(exported).next();
    for (auto const& guard : command.guards) {
        guards.push(guard);
    }
    auto inputs = std::move(guards).next();
    push_operands(inputs, command.inputs);
    auto outputs = std::move(inputs).next();
    push_operands(outputs, command.outputs);
    auto order_only_inputs = std::move(outputs).next();
    push_operands(order_only_inputs, command.order_only_inputs);
    return graph::close_command(graph, id, std::move(order_only_inputs));
}

} // namespace pup::test
