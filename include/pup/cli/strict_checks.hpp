// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/dag.hpp"
#include "pup/parser/ast.hpp"

#include <cstddef>

namespace pup::cli {

struct Diagnostic {
    StringId file = StringId::Empty;
    std::size_t line = 0;
    enum Severity { Warning,
                    Error } severity
        = Warning;
    StringId message = StringId::Empty;
};

/// Check an assignment statement for convention violations.
/// Only produces diagnostics for component Tuprules.tup files (not root).
[[nodiscard]]
auto check_assignment(
    parser::Assignment const& stmt,
    std::string_view file,
    bool is_component
) -> Vec<Diagnostic>;

/// Check component directories for filesystem-level conventions.
[[nodiscard]]
auto check_component_dirs(
    Vec<std::string_view> const& component_dirs
) -> Vec<Diagnostic>;

/// Report rules that produce object files no dependency scan covers.
/// Declining to scan is often correct — putup cannot reproduce an arbitrary command's shell
/// state — but declining in silence leaves the rule's headers unrecorded (#352).
[[nodiscard]]
auto check_unscanned_compiles(
    graph::Graph const& graph,
    graph::PathCache& cache
) -> Vec<Diagnostic>;

} // namespace pup::cli
