// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/path_id.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"

namespace pup::graph {

/// Information about a matched command for rule generation
struct CommandInfo {
    // check_unscanned_compiles calls match_and_generate with only command/node_id/working_dir populated; a scanner consuming other fields must extend that check.
    NodeId node_id;
    StringId command = StringId::Empty;
    StringId display = StringId::Empty;
    Vec<StringId> inputs;
    Vec<StringId> order_only_inputs;
    Vec<PathId> outputs;
    StringId working_dir = StringId::Empty;
};

/// A dependency scan of one command: its stdout is parsed as a depfile and the headers it names become that command's implicit dependencies
struct GeneratedRule {
    Vec<StringId> inputs;
    Vec<StringId> order_only_inputs; ///< Order-only deps (e.g., gen-headers)
    StringId command = StringId::Empty;
    StringId display = StringId::Empty;
    StringId covered_object = StringId::Empty; ///< The parent's object this rule records headers for
};

} // namespace pup::graph
