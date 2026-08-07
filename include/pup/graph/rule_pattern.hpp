// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/function.hpp"
#include "pup/core/path_id.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"

#include <optional>
#include <string_view>

namespace pup::graph {

/// Information about a matched command for rule generation
struct CommandInfo {
    // check_unscanned_compiles calls match_and_generate with only command/node_id/working_dir populated; a scanner consuming other fields must extend that check.
    NodeId node_id = INVALID_NODE_ID;
    StringId command = StringId::Empty;
    StringId display = StringId::Empty;
    Vec<StringId> inputs;
    Vec<StringId> order_only_inputs;
    Vec<PathId> outputs;
    StringId working_dir = StringId::Empty;
};

/// Output specification for generated rules
struct GeneratedOutput {
    enum class Type : std::uint8_t {
        File,   ///< Regular file output
        Stdout, ///< Capture stdout
        Stderr, ///< Capture stderr
    };

    Type type = Type::File;
    StringId path = StringId::Empty; ///< For File type: output path
};

/// What to do with the output of a generated rule
enum class OutputAction : std::uint8_t {
    Normal,            ///< Regular file output
    InjectImplicitDeps ///< Parse as depfile, add edges to parent command
};

/// A generated rule (same structure as user-defined rules)
struct GeneratedRule {
    Vec<StringId> inputs;
    Vec<StringId> order_only_inputs; ///< Order-only deps (e.g., gen-headers)
    StringId command = StringId::Empty;
    StringId display = StringId::Empty;
    Vec<GeneratedOutput> outputs;
    OutputAction action = OutputAction::Normal;
    NodeId parent_command = INVALID_NODE_ID; ///< For InjectImplicitDeps
};

/// Pattern that generates additional rules when matched
struct RulePattern {
    bool (*matches)(std::string_view command);

    /// Generate a rule from a matched command
    /// Returns nullopt if pattern matches but rule shouldn't be generated
    Function<std::optional<GeneratedRule>(CommandInfo const&)> generate;
};

/// Registry for rule patterns
class RulePatternRegistry final {
public:
    RulePatternRegistry() = default;

    /// Register a pattern
    auto register_pattern(RulePattern pattern) -> void;

    /// Check if a command matches any pattern and generate rules
    [[nodiscard]]
    auto match_and_generate(CommandInfo const& cmd) const
        -> Vec<GeneratedRule>;

    /// Check if registry has any patterns
    [[nodiscard]]
    auto empty() const -> bool
    {
        return patterns_.empty();
    }

    /// Get number of registered patterns
    [[nodiscard]]
    auto size() const -> std::size_t
    {
        return patterns_.size();
    }

private:
    Vec<RulePattern> patterns_;
};

/// Create the GCC/Clang depfile pattern
[[nodiscard]]
auto make_gcc_depfile_pattern() -> RulePattern;

} // namespace pup::graph
