// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/rule_pattern.hpp"

#include <memory>
#include <span>
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

/// One scan and the object file whose compile it reproduces. The object travels with the scan
/// because only the scanner's own parse of the invocation can say which one it is.
struct DepScan {
    StringId command = StringId::Empty;
    StringId object = StringId::Empty;
};

/// A command's words and the invocations they divide into, derived once and read by every scanner.
/// Obtainable only from `tokenize_command`, so the words can never disagree with the text they came
/// from; its views die with the frame that built it, which is why it is never stored anywhere.
class CommandTokens final {
public:
    friend auto tokenize_command(StringId text) -> CommandTokens;

    // Copying would leave the copy's invocation spans pointing into the source's word buffer, so
    // the value moves out of its constructor and is passed by reference from there on.
    CommandTokens(CommandTokens const&) = delete;
    auto operator=(CommandTokens const&) -> CommandTokens& = delete;
    CommandTokens(CommandTokens&&) = default;
    auto operator=(CommandTokens&&) -> CommandTokens& = default;
    ~CommandTokens() = default;

    [[nodiscard]]
    auto text() const -> StringId
    {
        return text_;
    }
    [[nodiscard]]
    auto words() const -> std::span<std::string_view const>
    {
        return { words_.data(), words_.size() };
    }
    [[nodiscard]]
    auto invocations() const -> std::span<std::span<std::string_view const> const>
    {
        return { invocations_.data(), invocations_.size() };
    }

private:
    CommandTokens() = default;

    StringId text_ = StringId::Empty;
    Vec<std::string_view> words_;
    Vec<std::span<std::string_view const>> invocations_;
};

/// The one way to obtain a CommandTokens.
[[nodiscard]]
auto tokenize_command(StringId text) -> CommandTokens;

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
    virtual auto matches(CommandInfo const& cmd, CommandTokens const& tokens) const -> bool = 0;

    /// Check if command already has dependency generation enabled
    [[nodiscard]]
    virtual auto has_dep_flags(CommandTokens const& tokens) const -> bool = 0;

    /// Build the scans that extract dependencies from the given command, one per compile
    /// invocation it can reproduce. Empty means the command carries no such invocation.
    [[nodiscard]]
    virtual auto build_dep_scans(
        CommandInfo const& cmd,
        CommandTokens const& tokens
    ) const -> Vec<DepScan> = 0;

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
    auto find_match(CommandInfo const& cmd, CommandTokens const& tokens) const -> DepScanner const*;

    /// Generate rules for a command using matching scanners
    [[nodiscard]]
    auto match_and_generate(CommandInfo const& cmd, CommandTokens const& tokens) const
        -> Vec<GeneratedRule>;

    /// Whether any scanner recognizes the command as writing its own depfile, which the
    /// build reads back from beside the object whether or not a scan was generated.
    [[nodiscard]]
    auto reports_own_deps(CommandTokens const& tokens) const -> bool;

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
