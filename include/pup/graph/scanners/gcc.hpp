// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/graph/dep_scanner.hpp"
#include "pup/graph/scanners/dep_words.hpp"

#include <memory>

namespace pup::graph::scanners {

/// Dependency scanner for GCC/Clang C/C++ compilers.
/// Detects gcc, g++, clang, clang++, cc, c++ commands with -c flag
/// and generates -M dependency scan commands.
class GccScanner final : public DepScanner {
public:
    [[nodiscard]]
    auto matches(CommandInfo const& cmd, CommandTokens const& tokens) const -> bool override;
    [[nodiscard]]
    auto has_dep_flags(CommandTokens const& tokens) const -> bool override;
    [[nodiscard]]
    auto build_dep_scans(CommandInfo const& cmd, CommandTokens const& tokens) const
        -> Vec<DepScan> override;
    [[nodiscard]]
    auto dep_spec() const -> DepSpec override;
    [[nodiscard]]
    auto name() const -> std::string_view override
    {
        return "gcc";
    }
};

/// Create a GCC/Clang scanner instance
[[nodiscard]]
auto make_gcc_scanner() -> std::unique_ptr<DepScanner>;

/// The scanner's flag tables. A test seam: the suite walks them so a flag added without an
/// argument policy fails there instead of dropping its word at scan time. No production caller.
[[nodiscard]]
auto gcc_flag_tables() -> FlagTables;

} // namespace pup::graph::scanners
