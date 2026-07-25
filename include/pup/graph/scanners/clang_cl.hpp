// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"
#include "pup/graph/dep_scanner.hpp"

#include <memory>

namespace pup::graph::scanners {

/// Dependency scanner for clang's MSVC-compatible driver (clang-cl).
/// Detects clang-cl commands with -c//c and generates `/clang:-M` scan
/// commands, which emit a GNU-format depfile on stdout.
class ClangClScanner final : public DepScanner {
public:
    [[nodiscard]]
    auto matches(CommandInfo const& cmd) const -> bool override;
    [[nodiscard]]
    auto has_dep_flags(std::string_view cmd) const -> bool override;
    [[nodiscard]]
    auto build_dep_command(CommandInfo const& cmd) const
        -> std::optional<StringId> override;
    [[nodiscard]]
    auto dep_spec() const -> DepSpec override;
    [[nodiscard]]
    auto name() const -> std::string_view override
    {
        return "clang-cl";
    }
};

/// Create a clang-cl scanner instance
[[nodiscard]]
auto make_clang_cl_scanner() -> std::unique_ptr<DepScanner>;

/// Check if a command string is a clang-cl compile command (driver + -c//c).
[[nodiscard]]
auto matches_clang_cl_compile(std::string_view command) -> bool;

// TODO: Add MsvcScanner for cl.exe using /showIncludes (stdout) or /sourceDependencies (file)

} // namespace pup::graph::scanners
