// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "ast.hpp"
#include "pup/core/result.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pup::parser {

/// Variable database for storing and retrieving variable values
class VarDb {
public:
    VarDb() = default;

    /// Set a variable value (replaces existing)
    auto set(std::string_view name, std::string value) -> void;

    /// Append to a variable value (space-separated)
    auto append(std::string_view name, std::string_view value) -> void;

    /// Get a variable value (returns empty if not found)
    [[nodiscard]] auto get(std::string_view name) const -> std::string_view;

    /// Check if variable exists
    [[nodiscard]] auto contains(std::string_view name) const -> bool;

    /// Remove a variable
    auto remove(std::string_view name) -> void;

    /// Get all variable names
    [[nodiscard]] auto names() const -> std::vector<std::string_view>;

    /// Clear all variables
    auto clear() -> void;

private:
    std::unordered_map<std::string, std::string> vars_;
};

/// Context for evaluating expressions
struct EvalContext {
    VarDb* vars = nullptr;           ///< Regular variables $(VAR)
    VarDb* config_vars = nullptr;    ///< Config variables @(VAR) from tup.config
    VarDb* node_vars = nullptr;      ///< Node variables &(VAR)

    std::string tup_cwd = {};        ///< Current directory (TUP_CWD)
    std::string tup_platform = {};   ///< Platform name (TUP_PLATFORM)
    std::string tup_arch = {};       ///< Architecture (TUP_ARCH)

    /// Callback for resolving group references like {groupname}
    std::function<std::vector<std::string>(std::string_view)> resolve_group = {};

    /// Callback for resolving bin references like <binname>
    std::function<std::vector<std::string>(std::string_view)> resolve_bin = {};
};

/// Pattern flags for command/output expansion
struct PatternFlags {
    std::string input = {};      ///< %f - input filename
    std::string input_base = {}; ///< %b - input basename (no path)
    std::string input_noext = {};///< %B - input basename without extension
    std::string input_ext = {};  ///< %e - input extension
    std::string output = {};     ///< %o - output filename
    std::string output_base = {};///< %O - output basename (no path)
    std::string input_dir = {};  ///< %d - input directory
    int input_index = 0;    ///< For %Nf patterns (1-indexed)
    std::vector<std::string> all_inputs = {}; ///< All inputs for %f expansion
};

/// Expression evaluator
class Evaluator {
public:
    explicit Evaluator(EvalContext& ctx);

    /// Expand an expression, replacing variable references with values
    [[nodiscard]] auto expand(Expression const& expr) -> Result<std::string>;

    /// Expand a string with variable references
    [[nodiscard]] auto expand(std::string_view text) -> Result<std::string>;

    /// Expand pattern flags (%f, %o, %B, etc.) in a string
    [[nodiscard]] auto expand_pattern(std::string_view text, PatternFlags const& flags)
        -> Result<std::string>;

    /// Expand a path pattern (handles globs, groups, exclusions)
    [[nodiscard]] auto expand_path(PathPattern const& pattern)
        -> Result<std::vector<std::string>>;

    /// Check if a conditional is true
    [[nodiscard]] auto evaluate_condition(Conditional const& cond) -> bool;

private:
    EvalContext& ctx_;

    [[nodiscard]] auto expand_var(VarRef const& ref) -> Result<std::string>;
    [[nodiscard]] auto expand_special_var(std::string_view name) -> std::optional<std::string>;
};

/// Built-in variable names
namespace builtin_vars {
constexpr auto TUP_CWD = "TUP_CWD";
constexpr auto TUP_PLATFORM = "TUP_PLATFORM";
constexpr auto TUP_ARCH = "TUP_ARCH";
constexpr auto TUP_VARIANTDIR = "TUP_VARIANTDIR";
constexpr auto CONFIG_ = "CONFIG_"; // Prefix for @() variables
} // namespace builtin_vars

} // namespace pup::parser
