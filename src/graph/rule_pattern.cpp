// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/rule_pattern.hpp"

#include "pup/core/global_pool.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/scanners/gcc.hpp"
#include <optional>
#include <utility>

namespace pup::graph {

auto RulePatternRegistry::register_pattern(RulePattern pattern) -> void
{
    patterns_.push_back(std::move(pattern));
}

auto RulePatternRegistry::match_and_generate(CommandInfo const& cmd) const
    -> Vec<GeneratedRule>
{
    auto result = Vec<GeneratedRule> {};
    auto cmd_sv = global_pool().get(cmd.command);

    for (auto const& pattern : patterns_) {
        if (!pattern.matches(cmd_sv)) {
            continue;
        }

        if (auto rule = pattern.generate(cmd)) {
            result.push_back(std::move(*rule));
        }
    }

    return result;
}

auto make_gcc_depfile_pattern() -> RulePattern
{
    return RulePattern {
        .matches = scanners::matches_gcc_compile,

        .generate = [](CommandInfo const& cmd) -> std::optional<GeneratedRule> {
            static auto const scanner = scanners::GccScanner {};

            if (scanner.has_dep_flags(global_pool().get(cmd.command))) {
                return std::nullopt;
            }

            auto dep_cmd = scanner.build_dep_command(cmd);
            if (!dep_cmd) {
                return std::nullopt;
            }

            return GeneratedRule {
                .inputs = cmd.inputs,
                .order_only_inputs = cmd.order_only_inputs,
                .command = *dep_cmd,
                .display = make_dep_display(cmd.inputs),
                .outputs = { { .type = GeneratedOutput::Type::Stdout, .path = StringId::Empty } },
                .action = OutputAction::InjectImplicitDeps,
                .parent_command = cmd.node_id,
            };
        },
    };
}

} // namespace pup::graph
