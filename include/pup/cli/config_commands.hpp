// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/node_id_map.hpp"
#include "pup/core/string.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/dag.hpp"

#include <string>

namespace pup::cli {

/// A command that outputs a tup.config file.
/// Used by both `pup configure` (runs these commands) and `pup build`
/// (errors if outputs don't exist).
struct ConfigCommand {
    NodeId cmd_id;
    String output_path; ///< Project-root-relative path
    bool exists;        ///< Whether output exists on disk (used by build, ignored by configure)
};

/// Find all commands that output tup.config files.
/// Paths in the graph are project-root-relative, so source_root is used
/// to resolve the full filesystem path for existence checks.
auto find_config_commands(
    graph::BuildGraph const& graph,
    std::string_view source_root
) -> Vec<ConfigCommand>;

/// Collect all commands that the given commands depend on (transitively).
auto collect_command_dependencies(
    graph::BuildGraph const& graph,
    NodeIdMap32 const& commands
) -> NodeIdMap32;

} // namespace pup::cli
