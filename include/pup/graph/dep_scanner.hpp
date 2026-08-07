// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/rule_pattern.hpp"

#include <memory>
#include <optional>
#include <string_view>

namespace pup::graph {

/// How to capture dependency output from a tool
enum class DepOutputMode : std::uint8_t {
    Stdout, ///< Parse stdout as depfile (e.g., gcc -M)
};

/// Specification for dependency extraction
struct DepSpec {
    DepOutputMode output_mode = DepOutputMode::Stdout;
};

/// Abstract interface for dependency scanners.
/// Implementations detect specific tools (compilers, assemblers, linkers)
/// and generate commands to extract their implicit dependencies.
class DepScanner {
public:
    virtual ~DepScanner() = default;

    DepScanner() = default;
    DepScanner(DepScanner const&) = default;
    DepScanner(DepScanner&&) = default;
    auto operator=(DepScanner const&) -> DepScanner& = default;
    auto operator=(DepScanner&&) -> DepScanner& = default;

    /// Check if this scanner applies to the given command
    [[nodiscard]]
    virtual auto matches(CommandInfo const& cmd) const -> bool = 0;

    /// Check if command already has dependency generation enabled
    [[nodiscard]]
    virtual auto has_dep_flags(std::string_view cmd) const -> bool = 0;

    /// Build a command to extract dependencies from the given command.
    /// Returns nullopt if deps shouldn't be extracted (e.g., already has flags).
    [[nodiscard]]
    virtual auto build_dep_command(
        CommandInfo const& cmd
    ) const -> std::optional<StringId> = 0;

    /// Get the dependency extraction specification
    [[nodiscard]]
    virtual auto dep_spec() const -> DepSpec = 0;

    /// Human-readable name for display/debugging
    [[nodiscard]]
    virtual auto name() const -> std::string_view = 0;
};

/// Build display string for DEP commands (e.g., "DEP foo.c")
[[nodiscard]]
auto make_dep_display(Vec<StringId> const& inputs) -> StringId;

/// Registry for dependency scanners.
/// Scanners are checked in registration order; first match wins.
class DepScannerRegistry final {
public:
    DepScannerRegistry() = default;

    /// Register a scanner
    auto register_scanner(std::unique_ptr<DepScanner> scanner) -> void;

    /// Find a scanner that matches the command (nullptr if none)
    [[nodiscard]]
    auto find_match(CommandInfo const& cmd) const -> DepScanner const*;

    /// Generate rules for a command using matching scanners
    [[nodiscard]]
    auto match_and_generate(CommandInfo const& cmd) const
        -> Vec<GeneratedRule>;

    /// Whether any scanner recognizes the command as writing its own depfile, which the
    /// build reads back from beside the object whether or not a scan was generated.
    [[nodiscard]]
    auto reports_own_deps(std::string_view cmd) const -> bool;

    [[nodiscard]]
    auto empty() const -> bool
    {
        return scanners_.empty();
    }
    [[nodiscard]]
    auto size() const -> std::size_t
    {
        return scanners_.size();
    }

private:
    Vec<std::unique_ptr<DepScanner>> scanners_;
};

} // namespace pup::graph
