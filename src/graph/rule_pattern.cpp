// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/rule_pattern.hpp"

#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/scanners/gcc.hpp"

namespace pup::graph {

auto RulePatternRegistry::register_pattern(RulePattern pattern) -> void
{
    patterns_.push_back(std::move(pattern));
}

auto RulePatternRegistry::match_and_generate(CommandInfo const& cmd) const
    -> std::vector<GeneratedRule>
{
    auto result = std::vector<GeneratedRule> {};

    for (auto const& pattern : patterns_) {
        if (!std::regex_search(cmd.command, pattern.command_pattern)) {
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
        .command_pattern = std::regex { R"((gcc|g\+\+|clang|clang\+\+|cc|c\+\+).*\s-c\s)" },

        .generate = [](CommandInfo const& cmd) -> std::optional<GeneratedRule> {
            static auto const scanner = scanners::GccScanner {};

            if (scanner.has_dep_flags(cmd.command)) {
                return std::nullopt;
            }

            auto dep_cmd = scanner.build_dep_command(cmd);
            if (!dep_cmd) {
                return std::nullopt;
            }

            return GeneratedRule {
                .inputs = cmd.inputs,
                .order_only_inputs = cmd.order_only_inputs,
                .command = std::move(*dep_cmd),
                .display = make_dep_display(cmd.inputs),
                .outputs = { { .type = GeneratedOutput::Type::Stdout, .path = {} } },
                .action = OutputAction::InjectImplicitDeps,
                .parent_command = cmd.node_id,
            };
        },
    };
}

} // namespace pup::graph
