// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/graph/dep_scanner.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/string_utils.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/rule_pattern.hpp"
#include "pup/graph/scanners/dep_words.hpp"
#include <cstddef>
#include <memory>
#include <span>
#include <utility>

namespace pup::graph {

auto tokenize_command(StringId text) -> CommandTokens
{
    auto& pool = global_pool();
    auto tokens = CommandTokens {};
    tokens.text_ = text;
    auto operator_positions = Vec<std::size_t> {};
    for (auto const& token : core::tokenize_shell_command(pool.get(text))) {
        switch (token.kind) {
        case core::ShellTokenKind::Word:
            tokens.words_.push_back(pool.get(token.text));
            break;
        case core::ShellTokenKind::Operator:
            operator_positions.push_back(tokens.words_.size());
            break;
        }
    }
    auto const words = std::span { tokens.words_.data(), tokens.words_.size() };
    auto start = std::size_t { 0 };
    for (auto end : operator_positions) {
        tokens.invocations_.push_back(words.subspan(start, end - start));
        start = end;
    }
    if (start < words.size() || tokens.invocations_.empty()) {
        tokens.invocations_.push_back(words.subspan(start));
    }
    return tokens;
}

auto make_dep_display(Vec<StringId> const& inputs) -> StringId
{
    if (inputs.empty()) {
        return global_pool().intern("DEP");
    }
    auto buf = Buf {};
    buf.append("DEP ");
    buf.append(global_pool().get(inputs[0]));
    return buf.intern(global_pool());
}

auto DepScannerRegistry::register_scanner(std::unique_ptr<DepScanner> scanner) -> void
{
    scanners_.push_back(std::move(scanner));
}

auto DepScannerRegistry::find_match(CommandInfo const& cmd, CommandTokens const& tokens) const
    -> DepScanner const*
{
    for (auto const& scanner : scanners_) {
        if (scanner->matches(cmd, tokens)) {
            return scanner.get();
        }
    }
    return nullptr;
}

auto DepScannerRegistry::reports_own_deps(CommandTokens const& tokens) const -> bool
{
    for (auto const& scanner : scanners_) {
        if (scanner->has_dep_flags(tokens)) {
            return true;
        }
    }
    return false;
}

auto DepScannerRegistry::match_and_generate(CommandInfo const& cmd, CommandTokens const& tokens) const
    -> Vec<GeneratedRule>
{
    auto result = Vec<GeneratedRule> {};

    for (auto const& scanner : scanners_) {
        if (!scanner->matches(cmd, tokens)) {
            continue;
        }

        if (scanner->has_dep_flags(tokens)) {
            continue;
        }

        auto spec = scanner->dep_spec();

        for (auto const& scan : scanner->build_dep_scans(cmd, tokens)) {
            auto outputs = Vec<GeneratedOutput> {};
            if (spec.output_mode == DepOutputMode::Stdout) {
                outputs.push_back({ .type = GeneratedOutput::Type::Stdout, .path = StringId::Empty });
            }

            result.push_back(GeneratedRule {
                .inputs = cmd.inputs,
                .order_only_inputs = cmd.order_only_inputs,
                .command = scan.command,
                .display = make_dep_display(cmd.inputs),
                .outputs = std::move(outputs),
                .action = OutputAction::InjectImplicitDeps,
                .parent_command = cmd.node_id,
                .covered_object = scan.object,
            });
        }
    }

    return result;
}

} // namespace pup::graph
