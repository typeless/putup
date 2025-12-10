// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "pup/core/types.hpp"

#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace pup::graph {

/// Information about a matched command for rule generation
struct CommandInfo {
    NodeId node_id = INVALID_NODE_ID;
    std::string command;
    std::string display;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::string working_dir;
};

/// Output specification for generated rules
struct GeneratedOutput {
    enum class Type : std::uint8_t {
        File,   ///< Regular file output
        Stdout, ///< Capture stdout
        Stderr, ///< Capture stderr
    };

    Type type = Type::File;
    std::string path; ///< For File type: output path
};

/// What to do with the output of a generated rule
enum class OutputAction : std::uint8_t {
    Normal,            ///< Regular file output
    InjectImplicitDeps ///< Parse as depfile, add edges to parent command
};

/// A generated rule (same structure as user-defined rules)
struct GeneratedRule {
    std::vector<std::string> inputs;
    std::string command;
    std::string display;
    std::vector<GeneratedOutput> outputs;
    OutputAction action = OutputAction::Normal;
    NodeId parent_command = INVALID_NODE_ID; ///< For InjectImplicitDeps
};

/// Pattern that generates additional rules when matched
struct RulePattern {
    std::regex command_pattern;

    /// Generate a rule from a matched command
    /// Returns nullopt if pattern matches but rule shouldn't be generated
    std::function<std::optional<GeneratedRule>(CommandInfo const&)> generate;
};

/// Registry for rule patterns
class RulePatternRegistry {
public:
    RulePatternRegistry() = default;

    /// Register a pattern
    auto register_pattern(RulePattern pattern) -> void;

    /// Check if a command matches any pattern and generate rules
    [[nodiscard]] auto match_and_generate(CommandInfo const& cmd) const
        -> std::vector<GeneratedRule>;

    /// Check if registry has any patterns
    [[nodiscard]] auto empty() const -> bool { return patterns_.empty(); }

    /// Get number of registered patterns
    [[nodiscard]] auto size() const -> std::size_t { return patterns_.size(); }

private:
    std::vector<RulePattern> patterns_;
};

/// Create the GCC/Clang depfile pattern
[[nodiscard]] auto make_gcc_depfile_pattern() -> RulePattern;

} // namespace pup::graph
