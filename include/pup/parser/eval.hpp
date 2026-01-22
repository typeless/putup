// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "ast.hpp"
#include "pup/core/result.hpp"

#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pup::parser {

/// Transparent hash for heterogeneous lookup in VarDb
struct StringHash {
    using is_transparent = void;
    auto operator()(std::string_view sv) const noexcept -> std::size_t
    {
        return std::hash<std::string_view> {}(sv);
    }
    auto operator()(std::string const& s) const noexcept -> std::size_t
    {
        return std::hash<std::string_view> {}(s);
    }
};

/// Variable database for storing and retrieving variable values
class VarDb {
public:
    VarDb() = default;

    /// Set a variable value (replaces existing)
    auto set(std::string_view name, std::string value) -> void;

    /// Append to a variable value (space-separated)
    auto append(std::string_view name, std::string_view value) -> void;

    /// Get a variable value (returns empty if not found)
    [[nodiscard]]
    auto get(std::string_view name) const -> std::string_view;

    /// Check if variable exists
    [[nodiscard]]
    auto contains(std::string_view name) const -> bool;

    /// Get all variable names
    [[nodiscard]]
    auto names() const -> std::vector<std::string_view>;

    /// Clear all variables
    auto clear() -> void;

private:
    std::unordered_map<std::string, std::string, StringHash, std::equal_to<>> vars_;
};

/// Context for evaluating expressions
struct EvalContext {
    VarDb* vars = nullptr;              ///< Regular variables $(VAR)
    VarDb const* config_vars = nullptr; ///< Config variables @(VAR) from tup.config (read-only)
    VarDb* node_vars = nullptr;         ///< Node variables &(VAR)

    std::string tup_cwd = {};               ///< Current directory (TUP_CWD)
    std::string tup_platform = {};          ///< Platform name (TUP_PLATFORM)
    std::string tup_arch = {};              ///< Architecture (TUP_ARCH)
    std::string tup_variantdir = {};        ///< Relative path to variant output (TUP_VARIANTDIR)
    std::string tup_variant_outputdir = {}; ///< Stable variant root path (TUP_VARIANT_OUTPUTDIR)
    std::string tup_srcdir = {};            ///< Relative path to source dir (TUP_SRCDIR)
    std::string tup_outdir = {};            ///< Relative path to output dir (TUP_OUTDIR)

    /// Callback for resolving group references like {groupname} (tup calls these "bins")
    std::function<std::vector<std::string>(std::string_view)> resolve_group = {};

    /// Callback for resolving order-only group references like <groupname>
    std::function<std::vector<std::string>(std::string_view)> resolve_order_only_group = {};

    /// Callback for requesting a directory's Tupfile to be parsed (for cross-directory deps)
    /// Called when a path references another directory that may have a Tupfile.
    /// Returns success if directory was parsed, error if circular/missing.
    std::function<Result<void>(std::filesystem::path const&)> request_directory = {};

    /// Set of directories that have Tupfiles (relative to root)
    /// Used to determine when to invoke request_directory callback
    std::set<std::filesystem::path> const* available_tupfile_dirs = nullptr;

    /// Callback for tracking config variable usage (for fine-grained dependency tracking)
    /// Called with the stripped variable name (e.g., "OPT" not "CONFIG_OPT") when
    /// a config variable is accessed via @(VAR) or $(CONFIG_VAR).
    std::function<void(std::string_view name)> on_config_var_used = {};

    /// Set of imported environment variable names (for tracking which vars are imported)
    std::unordered_set<std::string> const* imported_vars = nullptr;

    /// Callback for tracking imported env variable usage (for fine-grained dependency tracking)
    /// Called with the variable name when an imported env var is accessed via $(VAR).
    std::function<void(std::string_view name)> on_env_var_used = {};
};

/// Pattern flags for command/output expansion
struct PatternFlags {
    std::string input = {};                    ///< %f - input filename
    std::string input_base = {};               ///< %b - input basename (no path)
    std::string input_noext = {};              ///< %B - input basename without extension
    std::string input_ext = {};                ///< %e - input extension
    std::string output = {};                   ///< %o - output filename
    std::string output_base = {};              ///< %O - output basename (no path)
    std::string input_dir = {};                ///< %d - input directory
    std::string glob_match = {};               ///< %g - portion matched by * in foreach glob
    int input_index = 0;                       ///< For %Nf patterns (1-indexed)
    std::vector<std::string> all_inputs = {};  ///< All inputs for %Nf expansion
    std::vector<std::string> all_outputs = {}; ///< All outputs for %No expansion
};

/// Expression evaluator
class Evaluator {
public:
    explicit Evaluator(EvalContext* ctx);

    /// Expand an expression, replacing variable references with values
    [[nodiscard]]
    auto expand(Expression const& expr) -> Result<std::string>;

    /// Expand a string with variable references
    [[nodiscard]]
    auto expand(std::string_view text) -> Result<std::string>;

    /// Expand pattern flags (%f, %o, %B, etc.) in a string
    [[nodiscard]]
    auto expand_pattern(
        std::string_view text,
        PatternFlags const& flags
    ) -> Result<std::string>;

    /// Expand a path pattern (handles globs, groups, exclusions)
    [[nodiscard]]
    auto expand_path(
        PathPattern const& pattern
    ) -> Result<std::vector<std::string>>;

    /// Check if a conditional is true
    [[nodiscard]]
    auto evaluate_condition(Conditional const& cond) -> bool;

private:
    EvalContext* ctx_;

    [[nodiscard]]
    auto expand_var(VarRef const& ref) -> Result<std::string>;
    [[nodiscard]]
    auto expand_special_var(std::string_view name) -> std::optional<std::string>;
};

/// Built-in variable names
namespace builtin_vars {
constexpr auto TUP_CWD = "TUP_CWD";
constexpr auto TUP_PLATFORM = "TUP_PLATFORM";
constexpr auto TUP_ARCH = "TUP_ARCH";
constexpr auto TUP_VARIANTDIR = "TUP_VARIANTDIR";
constexpr auto TUP_VARIANT_OUTPUTDIR = "TUP_VARIANT_OUTPUTDIR";
constexpr auto TUP_SRCDIR = "TUP_SRCDIR";
constexpr auto TUP_OUTDIR = "TUP_OUTDIR";
constexpr auto CONFIG_ = "CONFIG_"; // Prefix for @() variables
} // namespace builtin_vars

} // namespace pup::parser
