// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/parser/eval.hpp"

#include <algorithm>
#include <charconv>

namespace pup::parser {

// =============================================================================
// VarDb
// =============================================================================

auto VarDb::set(std::string_view name, std::string value) -> void
{
    vars_[std::string{name}] = std::move(value);
}

auto VarDb::append(std::string_view name, std::string_view value) -> void
{
    auto key = std::string{name};
    auto it = vars_.find(key);
    if (it == vars_.end()) {
        vars_[key] = std::string{value};
    } else {
        if (!it->second.empty())
            it->second += ' ';
        it->second += value;
    }
}

auto VarDb::get(std::string_view name) const -> std::string_view
{
    auto it = vars_.find(std::string{name});
    if (it != vars_.end())
        return it->second;
    return {};
}

auto VarDb::contains(std::string_view name) const -> bool
{
    return vars_.find(std::string{name}) != vars_.end();
}

auto VarDb::remove(std::string_view name) -> void
{
    vars_.erase(std::string{name});
}

auto VarDb::names() const -> std::vector<std::string_view>
{
    auto result = std::vector<std::string_view>{};
    result.reserve(vars_.size());
    for (auto const& [name, _] : vars_)
        result.push_back(name);
    return result;
}

auto VarDb::clear() -> void
{
    vars_.clear();
}

// =============================================================================
// Evaluator
// =============================================================================

Evaluator::Evaluator(EvalContext& ctx)
    : ctx_(ctx)
{
}

auto Evaluator::expand(Expression const& expr) -> Result<std::string>
{
    auto result = std::string{};

    for (auto const& part : expr.parts) {
        if (std::holds_alternative<Expression::Literal>(part)) {
            result += std::get<Expression::Literal>(part).value;
        } else if (std::holds_alternative<Expression::Variable>(part)) {
            auto const& var = std::get<Expression::Variable>(part);
            auto expanded = expand_var(var.ref);
            if (!expanded)
                return pup::unexpected<Error>(expanded.error());
            result += *expanded;
        }
    }

    return result;
}

auto Evaluator::expand(std::string_view text) -> Result<std::string>
{
    auto result = std::string{};
    auto pos = std::size_t{0};

    while (pos < text.size()) {
        // Look for variable references
        auto dollar = text.find('$', pos);
        auto at = text.find('@', pos);
        auto amp = text.find('&', pos);

        // Find the earliest variable reference
        auto next = std::min({dollar, at, amp});

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

                if (text[next] == '@')
                    kind = VarRef::Kind::Config;
                else if (text[next] == '&')
                    kind = VarRef::Kind::Node;

                auto ref = VarRef{kind, std::string{name}, {}};
                auto expanded = expand_var(ref);
                if (!expanded)
                    return pup::unexpected<Error>(expanded.error());
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

auto Evaluator::expand_pattern(std::string_view text, PatternFlags const& flags)
    -> Result<std::string>
{
    auto result = std::string{};
    auto pos = std::size_t{0};

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
            while (end < text.size() && text[end] >= '0' && text[end] <= '9')
                ++end;

            auto num = int{0};
            auto start_ptr = text.data() + percent + 1;
            auto end_ptr = text.data() + end;
            std::from_chars(start_ptr, end_ptr, num);

            if (end < text.size() && text[end] == 'f') {
                // %Nf - N-th input file
                if (num > 0 && static_cast<std::size_t>(num) <= flags.all_inputs.size())
                    result += flags.all_inputs[static_cast<std::size_t>(num - 1)];
                pos = end + 1;
                continue;
            }

            // Not a valid pattern, output as-is
            result += '%';
            pos = percent + 1;
            continue;
        }

        // Standard pattern flags
        switch (flag) {
        case 'f':
            result += flags.input;
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
            // %g - group outputs (placeholder)
            break;
        case 'i':
            // %i - all inputs space-separated
            for (std::size_t i = 0; i < flags.all_inputs.size(); ++i) {
                if (i > 0)
                    result += ' ';
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

auto Evaluator::expand_path(PathPattern const& pattern)
    -> Result<std::vector<std::string>>
{
    auto result = std::vector<std::string>{};

    if (pattern.is_group) {
        // Group reference - use callback to resolve
        if (ctx_.resolve_group) {
            auto paths = ctx_.resolve_group(pattern.group_name);
            result.insert(result.end(), paths.begin(), paths.end());
        }
        return result;
    }

    // Expand the path expression
    auto path_result = expand(pattern.path);
    if (!path_result)
        return pup::unexpected<Error>(path_result.error());

    // For now, just return the expanded path
    // Glob expansion will be handled by the glob module
    result.push_back(*path_result);

    return result;
}

auto Evaluator::evaluate_condition(Conditional const& cond) -> bool
{
    switch (cond.kind) {
    case Conditional::Kind::Ifdef:
        if (ctx_.vars && ctx_.vars->contains(cond.var_name))
            return true;
        if (ctx_.config_vars && ctx_.config_vars->contains(cond.var_name))
            return true;
        return false;

    case Conditional::Kind::Ifndef:
        if (ctx_.vars && ctx_.vars->contains(cond.var_name))
            return false;
        if (ctx_.config_vars && ctx_.config_vars->contains(cond.var_name))
            return false;
        return true;

    case Conditional::Kind::Ifeq: {
        auto lhs = expand(cond.lhs);
        auto rhs = expand(cond.rhs);
        if (!lhs || !rhs)
            return false;
        return *lhs == *rhs;
    }

    case Conditional::Kind::Ifneq: {
        auto lhs = expand(cond.lhs);
        auto rhs = expand(cond.rhs);
        if (!lhs || !rhs)
            return false;
        return *lhs != *rhs;
    }
    }

    return false;
}

auto Evaluator::expand_var(VarRef const& ref) -> Result<std::string>
{
    // First check for special built-in variables
    if (auto special = expand_special_var(ref.name))
        return *special;

    VarDb* db = nullptr;
    switch (ref.kind) {
    case VarRef::Kind::Regular:
        db = ctx_.vars;
        break;
    case VarRef::Kind::Config:
        db = ctx_.config_vars;
        break;
    case VarRef::Kind::Node:
        db = ctx_.node_vars;
        break;
    }

    if (db) {
        auto value = db->get(ref.name);
        return std::string{value};
    }

    // Variable not found - return empty string (tup behavior)
    return std::string{};
}

auto Evaluator::expand_special_var(std::string_view name) -> std::optional<std::string>
{
    if (name == builtin_vars::TUP_CWD)
        return ctx_.tup_cwd;
    if (name == builtin_vars::TUP_PLATFORM)
        return ctx_.tup_platform;
    if (name == builtin_vars::TUP_ARCH)
        return ctx_.tup_arch;

    return std::nullopt;
}

} // namespace pup::parser
