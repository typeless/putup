// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/parser/eval.hpp"

#include "pup/core/platform.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>

namespace pup::parser {

// =============================================================================
// VarDb
// =============================================================================

auto VarDb::set(std::string_view name, std::string value) -> void
{
    vars_[std::string { name }] = std::move(value);
}

auto VarDb::append(std::string_view name, std::string_view value) -> void
{
    auto it = vars_.find(name); // Heterogeneous lookup
    if (it == vars_.end()) {
        vars_[std::string { name }] = std::string { value };
    } else {
        if (!it->second.empty()) {
            it->second += ' ';
        }
        it->second += value;
    }
}

auto VarDb::get(std::string_view name) const -> std::string_view
{
    auto it = vars_.find(name); // Heterogeneous lookup - no temp string
    if (it != vars_.end()) {
        return it->second;
    }
    return {};
}

auto VarDb::contains(std::string_view name) const -> bool
{
    return vars_.contains(name);
}

auto VarDb::names() const -> std::vector<std::string_view>
{
    auto result = std::vector<std::string_view> {};
    result.reserve(vars_.size());
    for (auto const& [name, _] : vars_) {
        result.push_back(name);
    }
    return result;
}

auto VarDb::clear() -> void
{
    vars_.clear();
}

// =============================================================================
// Evaluator
// =============================================================================

Evaluator::Evaluator(EvalContext* ctx)
    : ctx_(ctx)
{
}

auto Evaluator::expand(Expression const& expr) -> Result<std::string>
{
    auto result = std::string {};

    for (auto const& part : expr.parts) {
        if (std::holds_alternative<Expression::Literal>(part)) {
            result += std::get<Expression::Literal>(part).value;
        } else if (std::holds_alternative<Expression::Variable>(part)) {
            auto const& var = std::get<Expression::Variable>(part);
            auto expanded = expand_var(var.ref);
            if (!expanded) {
                return pup::unexpected<Error>(expanded.error());
            }
            result += *expanded;
        }
    }

    // Recursively expand any variable references that were embedded in literals
    // (e.g., from escaped quotes like \"$(VAR)\")
    return expand(std::string_view { result });
}

auto Evaluator::expand(std::string_view text) -> Result<std::string>
{
    auto result = std::string {};
    auto pos = std::size_t { 0 };

    while (pos < text.size()) {
        // Look for variable references
        auto dollar = text.find('$', pos);
        auto at = text.find('@', pos);
        auto amp = text.find('&', pos);

        // Find the earliest variable reference
        auto next = std::min({ dollar, at, amp });

        if (next == std::string_view::npos) {
            // No more variable references
            result += text.substr(pos);
            break;
        }

        // Add text before the variable
        result += text.substr(pos, next - pos);

        // Check for variable reference pattern: X(name)
        if (next + 1 < text.size() && text[next + 1] == '(') {
            auto close = text.find(')', next + 2);
            if (close != std::string_view::npos) {
                auto name = text.substr(next + 2, close - next - 2);
                auto kind = VarRef::Kind::Regular;

                if (text[next] == '@') {
                    kind = VarRef::Kind::Config;
                } else if (text[next] == '&') {
                    kind = VarRef::Kind::Node;
                }

                auto ref = VarRef { kind, std::string { name }, {} };
                auto expanded = expand_var(ref);
                if (!expanded) {
                    return pup::unexpected<Error>(expanded.error());
                }
                result += *expanded;
                pos = close + 1;
                continue;
            }
        }

        // Not a variable reference, just add the character
        result += text[next];
        pos = next + 1;
    }

    return result;
}

auto Evaluator::expand_pattern(
    std::string_view text,
    PatternFlags const& flags
) -> Result<std::string>
{
    auto result = std::string {};
    auto pos = std::size_t { 0 };

    while (pos < text.size()) {
        auto percent = text.find('%', pos);

        if (percent == std::string_view::npos) {
            result += text.substr(pos);
            break;
        }

        result += text.substr(pos, percent - pos);

        if (percent + 1 >= text.size()) {
            result += '%';
            pos = percent + 1;
            continue;
        }

        auto flag = text[percent + 1];
        pos = percent + 2;

        // Check for %% escape
        if (flag == '%') {
            result += '%';
            continue;
        }

        // Check for %Nf pattern (N-th input)
        if (flag >= '0' && flag <= '9') {
            auto end = pos;
            while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
                ++end;
            }

            auto num = 0;
            auto const* start_ptr = text.data() + percent + 1;
            auto const* end_ptr = text.data() + end;
            std::from_chars(start_ptr, end_ptr, num);

            if (end < text.size() && text[end] == 'f') {
                // %Nf - N-th input file
                if (num > 0 && static_cast<std::size_t>(num) <= flags.all_inputs.size()) {
                    result += flags.all_inputs[static_cast<std::size_t>(num - 1)];
                }
                pos = end + 1;
                continue;
            }

            if (end < text.size() && text[end] == 'o') {
                // %No - N-th output file
                if (num > 0 && static_cast<std::size_t>(num) <= flags.all_outputs.size()) {
                    result += flags.all_outputs[static_cast<std::size_t>(num - 1)];
                }
                pos = end + 1;
                continue;
            }

            // Not a valid pattern, output as-is
            result += '%';
            pos = percent + 1;
            continue;
        }

        // Check for %<group> pattern
        if (flag == '<') {
            auto end = text.find('>', pos);
            if (end == std::string_view::npos) {
                // No closing '>', output as-is
                result += "%<";
                continue;
            }
            auto group_name = std::string { text.substr(pos, end - pos) };
            pos = end + 1;

            // Resolve order-only group to paths via callback
            if (ctx_->resolve_order_only_group) {
                auto paths = ctx_->resolve_order_only_group(group_name);
                for (std::size_t i = 0; i < paths.size(); ++i) {
                    if (i > 0) {
                        result += ' ';
                    }
                    result += paths[i];
                }
            }
            continue;
        }

        // Standard pattern flags
        switch (flag) {
        case 'f':
            // %f - all inputs space-separated
            for (std::size_t i = 0; i < flags.all_inputs.size(); ++i) {
                if (i > 0) {
                    result += ' ';
                }
                result += flags.all_inputs[i];
            }
            break;
        case 'b':
            result += flags.input_base;
            break;
        case 'B':
            result += flags.input_noext;
            break;
        case 'e':
            result += flags.input_ext;
            break;
        case 'o':
            result += flags.output;
            break;
        case 'O':
            result += flags.output_base;
            break;
        case 'd':
            result += flags.input_dir;
            break;
        case 'g':
            result += flags.glob_match;
            break;
        case 'i':
            // %i - all inputs space-separated
            for (std::size_t i = 0; i < flags.all_inputs.size(); ++i) {
                if (i > 0) {
                    result += ' ';
                }
                result += flags.all_inputs[i];
            }
            break;
        default:
            // Unknown flag, keep as-is
            result += '%';
            result += flag;
            break;
        }
    }

    return result;
}

auto Evaluator::expand_path(
    PathPattern const& pattern
) -> Result<std::vector<std::string>>
{
    auto result = std::vector<std::string> {};

    if (pattern.is_order_only_group) {
        // Order-only group reference <groupname> - use callback to resolve
        if (ctx_->resolve_order_only_group) {
            auto paths = ctx_->resolve_order_only_group(pattern.group_name);
            result.insert(result.end(), paths.begin(), paths.end());
        }
        return result;
    }

    if (pattern.is_group) {
        // Group reference {groupname} - use callback to resolve
        if (ctx_->resolve_group) {
            auto paths = ctx_->resolve_group(pattern.group_name);
            result.insert(result.end(), paths.begin(), paths.end());
        }
        return result;
    }

    // Expand the path expression
    auto path_result = expand(pattern.path);
    if (!path_result) {
        return pup::unexpected<Error>(path_result.error());
    }

    // Split result by whitespace - variables may contain multiple files
    auto const& expanded = *path_result;
    auto start = std::size_t { 0 };
    while (start < expanded.size()) {
        // Skip whitespace
        while (start < expanded.size() && (expanded[start] == ' ' || expanded[start] == '\t')) {
            ++start;
        }
        if (start >= expanded.size()) {
            break;
        }
        // Find end of token
        auto end = start;
        while (end < expanded.size() && expanded[end] != ' ' && expanded[end] != '\t') {
            ++end;
        }
        if (end > start) {
            // Normalize path to remove // and resolve . and .. components
            auto path_str = expanded.substr(start, end - start);
            auto normalized = std::filesystem::path { path_str }.lexically_normal().string();
            result.push_back(std::move(normalized));
        }
        start = end;
    }

    return result;
}

auto Evaluator::evaluate_condition(Conditional const& cond) -> bool
{
    switch (cond.kind) {
    case Conditional::Kind::Ifdef:
        if (ctx_->vars && ctx_->vars->contains(cond.var_name)) {
            return true;
        }
        if (ctx_->config_vars && ctx_->config_vars->contains(cond.var_name)) {
            return true;
        }
        return false;

    case Conditional::Kind::Ifndef:
        if (ctx_->vars && ctx_->vars->contains(cond.var_name)) {
            return false;
        }
        if (ctx_->config_vars && ctx_->config_vars->contains(cond.var_name)) {
            return false;
        }
        return true;

    case Conditional::Kind::Ifeq: {
        auto lhs = expand(cond.lhs);
        auto rhs = expand(cond.rhs);
        if (!lhs || !rhs) {
            return false;
        }
        return *lhs == *rhs;
    }

    case Conditional::Kind::Ifneq: {
        auto lhs = expand(cond.lhs);
        auto rhs = expand(cond.rhs);
        if (!lhs || !rhs) {
            return false;
        }
        return *lhs != *rhs;
    }
    }

    return false;
}

auto Evaluator::expand_var(VarRef const& ref) -> Result<std::string>
{
    // TUP_PLATFORM and TUP_ARCH have special priority: env > config > default
    // Check env var first (highest priority)
    if (ref.name == builtin_vars::TUP_PLATFORM) {
        if (auto const* env = std::getenv("TUP_PLATFORM"); env && *env) {
            return std::string { env };
        }
    }
    if (ref.name == builtin_vars::TUP_ARCH) {
        if (auto const* env = std::getenv("TUP_ARCH"); env && *env) {
            return std::string { env };
        }
    }

    // Check for context-computed special variables (TUP_CWD, TUP_VARIANTDIR, etc.)
    // These are NOT overridable via config
    if (auto special = expand_special_var(ref.name)) {
        return *special;
    }

    VarDb const* db = nullptr;
    switch (ref.kind) {
    case VarRef::Kind::Regular:
        db = ctx_->vars;
        break;
    case VarRef::Kind::Config:
        db = ctx_->config_vars;
        break;
    case VarRef::Kind::Node:
        db = ctx_->node_vars;
        break;
    }

    if (db && db->contains(ref.name)) {
        // Track config variable usage for fine-grained dependency tracking
        // Track even for empty values - if value changes, command should rebuild
        if (ref.kind == VarRef::Kind::Config && ctx_->on_config_var_used) {
            ctx_->on_config_var_used(ref.name);
        }
        // Track imported env variable usage for fine-grained dependency tracking
        if (ref.kind == VarRef::Kind::Regular && ctx_->imported_vars
            && ctx_->imported_vars->contains(ref.name) && ctx_->on_env_var_used) {
            ctx_->on_env_var_used(ref.name);
        }
        return std::string { db->get(ref.name) };
    }

    // For regular variables, also check config_vars (tup behavior: CONFIG_* are accessible via $())
    if (ref.kind == VarRef::Kind::Regular && ctx_->config_vars && ctx_->config_vars->contains(ref.name)) {
        // Track config variable usage - strip CONFIG_ prefix if present
        // Track even for empty values - if value changes, command should rebuild
        if (ctx_->on_config_var_used) {
            auto name = ref.name;
            if (name.starts_with(builtin_vars::CONFIG_)) {
                name = name.substr(std::string_view { builtin_vars::CONFIG_ }.size());
            }
            ctx_->on_config_var_used(name);
        }
        return std::string { ctx_->config_vars->get(ref.name) };
    }

    // Fall back to compile-time default for TUP_PLATFORM/TUP_ARCH
    if (ref.name == builtin_vars::TUP_PLATFORM) {
        return std::string { pup::PLATFORM };
    }
    if (ref.name == builtin_vars::TUP_ARCH) {
        return std::string { pup::ARCH };
    }

    // Variable not found - return empty string (tup behavior)
    return std::string {};
}

auto Evaluator::expand_special_var(std::string_view name) -> std::optional<std::string>
{
    // Context-computed special variables - NOT overridable via config
    if (name == builtin_vars::TUP_CWD) {
        return ctx_->tup_cwd;
    }
    if (name == builtin_vars::TUP_VARIANTDIR) {
        return ctx_->tup_variantdir;
    }
    if (name == builtin_vars::TUP_VARIANT_OUTPUTDIR) {
        return ctx_->tup_variant_outputdir;
    }
    if (name == builtin_vars::TUP_SRCDIR) {
        return ctx_->tup_srcdir;
    }
    if (name == builtin_vars::TUP_OUTDIR) {
        return ctx_->tup_outdir;
    }

    // TUP_PLATFORM and TUP_ARCH are handled in expand_var() with proper priority:
    // env > config > default

    return std::nullopt;
}

} // namespace pup::parser
