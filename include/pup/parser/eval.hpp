// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "ast.hpp"
#include "pup/core/function.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/result.hpp"
#include "pup/core/sorted_id_vec.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/vec.hpp"

#include <string_view>

namespace pup::graph {
struct VarDepTracker;
}

namespace pup::parser {

/// Variable database for storing and retrieving variable values.
/// All interning goes through global_pool().
class VarDb {
public:
    VarDb() = default;
    ~VarDb() = default;

    VarDb(VarDb const& other);
    auto operator=(VarDb const& other) -> VarDb&;

    VarDb(VarDb&&) noexcept = default;
    auto operator=(VarDb&&) noexcept -> VarDb& = default;

    auto set(std::string_view name, std::string_view value) -> void;
    auto append(std::string_view name, std::string_view value) -> void;

    [[nodiscard]]
    auto get(std::string_view name) const -> std::string_view;
    [[nodiscard]]
    auto contains(std::string_view name) const -> bool;
    [[nodiscard]]
    auto names() const -> Vec<std::string_view>;

    auto clear() -> void;

private:
    SortedPairVec entries_;
};

/// Identifies which variable bank a lookup resolved from
enum class VarBank {
    Builtin,  ///< Built-in variable (TUP_CWD, TUP_PLATFORM, etc.)
    Regular,  ///< Regular variable $(VAR)
    Config,   ///< Config variable @(VAR) or $(CONFIG_VAR)
    Node,     ///< Node variable &(VAR)
    Env,      ///< Environment variable
    NotFound, ///< Variable not found
};

/// Variable context - groups variable sources for lookup
/// This is the "register bank" abstraction for variable resolution.
struct VarContext {
    VarDb* vars = nullptr;         ///< Regular variables $(VAR)
    VarDb const* config = nullptr; ///< Config variables @(VAR)
    VarDb* node = nullptr;         ///< Node variables &(VAR)

    std::string_view tup_cwd = {};               ///< TUP_CWD value
    std::string_view tup_platform = {};          ///< TUP_PLATFORM value
    std::string_view tup_arch = {};              ///< TUP_ARCH value
    std::string_view tup_variantdir = {};        ///< TUP_VARIANTDIR value
    std::string_view tup_variant_outputdir = {}; ///< TUP_VARIANT_OUTPUTDIR value
    std::string_view tup_srcdir = {};            ///< TUP_SRCDIR value
    std::string_view tup_outdir = {};            ///< TUP_OUTDIR value

    /// Set of imported variable name StringIds
    SortedIdVec const* imported_vars = nullptr;

    /// String pool for looking up StringIds (read-only during lookup)
    StringPool const* string_pool = nullptr;
};

/// Result of variable lookup with bank information
struct VarLookupResult {
    std::optional<std::string_view> value = std::nullopt;
    VarBank bank = VarBank::NotFound;
};

/// Lookup a variable by name with appropriate priority.
/// Priority: builtin > env (for TUP_PLATFORM/ARCH) > regular > config (with CONFIG_ prefix)
/// @param ctx Variable context containing all variable banks
/// @param name Variable name to look up
/// @param kind The kind of variable reference (affects lookup priority)
/// @return The variable value if found, or nullopt
[[nodiscard]]
auto lookup_var(VarContext const& ctx, std::string_view name, VarRef::Kind kind = VarRef::Kind::Regular)
    -> std::optional<std::string_view>;

/// Lookup a variable by name and return which bank it came from.
/// Useful for dependency tracking - knowing which bank was accessed.
/// @param ctx Variable context containing all variable banks
/// @param name Variable name to look up
/// @param kind The kind of variable reference (affects lookup priority)
/// @return Pair of optional value and the bank it came from
[[nodiscard]]
auto lookup_var_with_bank(VarContext const& ctx, std::string_view name, VarRef::Kind kind = VarRef::Kind::Regular)
    -> VarLookupResult;

/// Context for evaluating expressions.
///
/// Note: The variable-related fields (vars, config_vars, node_vars, tup_* strings)
/// parallel VarContext but differ in ownership semantics:
/// - EvalContext owns strings (String) for stable lifetime
/// - VarContext uses views (std::string_view) for efficient lookup
///
/// Use make_var_context() (internal) to create a VarContext from EvalContext.
struct EvalContext {
    VarDb* vars = nullptr;              ///< Regular variables $(VAR)
    VarDb const* config_vars = nullptr; ///< Config variables @(VAR) from tup.config (read-only)
    VarDb* node_vars = nullptr;         ///< Node variables &(VAR)

    StringId tup_cwd = StringId::Empty;               ///< Current directory (TUP_CWD)
    StringId tup_platform = StringId::Empty;          ///< Platform name (TUP_PLATFORM)
    StringId tup_arch = StringId::Empty;              ///< Architecture (TUP_ARCH)
    StringId tup_variantdir = StringId::Empty;        ///< Relative path to variant output (TUP_VARIANTDIR)
    StringId tup_variant_outputdir = StringId::Empty; ///< Stable variant root path (TUP_VARIANT_OUTPUTDIR)
    StringId tup_srcdir = StringId::Empty;            ///< Relative path to source dir (TUP_SRCDIR)
    StringId tup_outdir = StringId::Empty;            ///< Relative path to output dir (TUP_OUTDIR)

    /// Callback for resolving group references like {groupname} (tup calls these "bins")
    Function<Vec<StringId>(std::string_view)> resolve_group = {};

    /// Callback for resolving order-only group references like <groupname>
    Function<Vec<StringId>(std::string_view)> resolve_order_only_group = {};

    /// Callback for requesting a directory's Tupfile to be parsed (for cross-directory deps)
    /// Called when a path references another directory that may have a Tupfile.
    /// Returns success if directory was parsed, error if circular/missing.
    Function<Result<void>(std::string_view)> request_directory = {};

    /// Set of directories that have Tupfiles (relative to root)
    /// Used to determine when to invoke request_directory callback
    Vec<StringId> const* available_tupfile_dirs = nullptr;

    /// Callback for tracking config variable usage (for fine-grained dependency tracking)
    /// Called with the stripped variable name (e.g., "OPT" not "CONFIG_OPT") when
    /// a config variable is accessed via @(VAR) or $(CONFIG_VAR).
    Function<void(std::string_view name)> on_config_var_used = {};

    /// Set of imported variable name StringIds
    SortedIdVec const* imported_vars = nullptr;

    /// Callback for tracking imported env variable usage (for fine-grained dependency tracking)
    /// Called with the variable name when an imported env var is accessed via $(VAR).
    Function<void(std::string_view name)> on_env_var_used = {};

    /// Callback for strict convention checking.
    /// Called with each statement and the directory it belongs to.
    Function<void(Statement const&, std::string_view dir)> on_statement = {};

    /// String pool for resolving StringIds during variable lookup
    StringPool const* string_pool = nullptr;

    /// Tracker for transitive config var dependencies of regular variables.
    /// When a variable is assigned a value containing @(CONFIG_VAR), record the dependency.
    /// When that variable is later expanded, propagate the dependency via on_config_var_used.
    graph::VarDepTracker const* var_config_deps = nullptr;

    /// Tracker for transitive env var dependencies of regular variables.
    graph::VarDepTracker const* var_env_deps = nullptr;

    /// Callback for tracking variable assignments (for show var command)
    /// Called when a variable is assigned with the variable name, operator, before/after values,
    /// filename, line/column, and whether the assignment was effective.
    Function<void(
        std::string_view name,
        Assignment::Op op,
        std::string_view value_before,
        std::string_view value_after,
        std::string_view filename,
        std::uint32_t line,
        std::uint32_t column,
        bool is_effective
    )>
        on_var_assigned = {};
};

/// Pattern flags for command/output expansion
struct PatternFlags {
    std::string_view input = {};            ///< %f - input filename
    std::string_view input_base = {};       ///< %b - input basename (no path)
    std::string_view input_noext = {};      ///< %B - input basename without extension
    std::string_view input_ext = {};        ///< %e - input extension
    std::string_view output = {};           ///< %o - output filename
    std::string_view output_base = {};      ///< %O - output basename (no path)
    std::string_view input_dir = {};        ///< %d - input directory
    std::string_view glob_match = {};       ///< %g - portion matched by * in foreach glob
    int input_index = 0;                    ///< For %Nf patterns (1-indexed)
    Vec<std::string_view> all_inputs = {};  ///< All inputs for %Nf expansion
    Vec<std::string_view> all_outputs = {}; ///< All outputs for %No expansion
};

/// Expand an expression, replacing variable references with values
[[nodiscard]]
auto expand(EvalContext& ctx, Expression const& expr) -> Result<StringId>;

/// Expand a string with variable references
[[nodiscard]]
auto expand(EvalContext& ctx, std::string_view text) -> Result<StringId>;

/// Expand pattern flags (%f, %o, %B, etc.) in a string
[[nodiscard]]
auto expand_pattern(
    EvalContext& ctx,
    std::string_view text,
    PatternFlags const& flags
) -> Result<StringId>;

/// Expand a path pattern (handles globs, groups, exclusions)
[[nodiscard]]
auto expand_path(
    EvalContext& ctx,
    PathPattern const& pattern
) -> Result<Vec<StringId>>;

/// Check if a conditional is true
[[nodiscard]]
auto evaluate_condition(EvalContext& ctx, Conditional const& cond) -> bool;

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
