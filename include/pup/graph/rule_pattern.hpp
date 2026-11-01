// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/path_id.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"

#include <cstdint>

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
    NodeId parent_command = INVALID_NODE_ID;   ///< For InjectImplicitDeps
    StringId covered_object = StringId::Empty; ///< The parent's object this rule records headers for
};

} // namespace pup::graph
