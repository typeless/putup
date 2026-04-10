// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/dep_scanner.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path_pool.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/rule_pattern.hpp"
#include <memory>
#include <utility>

namespace pup::graph {

auto make_dep_display(Vec<PathId> const& inputs, PathPool const& paths) -> StringId
{
    if (inputs.empty()) {
        return global_pool().intern("DEP");
    }
    auto buf = Buf {};
    buf.append("DEP ");
    paths.write(inputs[0], buf, global_pool());
    return buf.intern(global_pool());
}

auto materialize_inputs(Vec<PathId> const& inputs, PathPool const& paths) -> Vec<StringId>
{
    auto& pool = global_pool();
    auto result = Vec<StringId> {};
    result.reserve(inputs.size());
    for (auto id : inputs) {
        result.push_back(paths.to_string(id, pool));
    }
    return result;
}

auto DepScannerRegistry::register_scanner(std::unique_ptr<DepScanner> scanner) -> void
{
    scanners_.push_back(std::move(scanner));
}

auto DepScannerRegistry::find_match(CommandInfo const& cmd) const -> DepScanner const*
{
    for (auto const& scanner : scanners_) {
        if (scanner->matches(cmd)) {
            return scanner.get();
        }
    }
    return nullptr;
}

auto DepScannerRegistry::match_and_generate(CommandInfo const& cmd) const
    -> Vec<GeneratedRule>
{
    auto result = Vec<GeneratedRule> {};

    for (auto const& scanner : scanners_) {
        if (!scanner->matches(cmd)) {
            continue;
        }

        if (scanner->has_dep_flags(global_pool().get(cmd.command))) {
            continue;
        }

        auto dep_cmd = scanner->build_dep_command(cmd);
        if (!dep_cmd) {
            continue;
        }

        auto spec = scanner->dep_spec();

        auto outputs = Vec<GeneratedOutput> {};
        if (spec.output_mode == DepOutputMode::Stdout) {
            outputs.push_back({ .type = GeneratedOutput::Type::Stdout, .path = StringId::Empty });
        }

        auto string_inputs = cmd.path_pool ? materialize_inputs(cmd.inputs, *cmd.path_pool) : Vec<StringId> {};
        result.push_back(GeneratedRule {
            .inputs = std::move(string_inputs),
            .order_only_inputs = cmd.order_only_inputs,
            .command = *dep_cmd,
            .display = cmd.path_pool ? make_dep_display(cmd.inputs, *cmd.path_pool) : global_pool().intern("DEP"),
            .outputs = std::move(outputs),
            .action = OutputAction::InjectImplicitDeps,
            .parent_command = cmd.node_id,
        });
    }

    return result;
}

} // namespace pup::graph
