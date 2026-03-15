// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/graph/dep_scanner.hpp"

#include <memory>

namespace pup::graph::scanners {

/// Dependency scanner for GCC/Clang C/C++ compilers.
/// Detects gcc, g++, clang, clang++, cc, c++ commands with -c flag
/// and generates -M dependency scan commands.
class GccScanner final : public DepScanner {
public:
    [[nodiscard]]
    auto matches(CommandInfo const& cmd) const -> bool override;
    [[nodiscard]]
    auto has_dep_flags(std::string const& cmd) const -> bool override;
    [[nodiscard]]
    auto build_dep_command(CommandInfo const& cmd) const
        -> std::optional<std::string> override;
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

// TODO: Add MsvcScanner for cl.exe using /showIncludes (stdout) or /sourceDependencies (file)

} // namespace pup::graph::scanners
