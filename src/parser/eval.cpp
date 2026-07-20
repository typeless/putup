// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/parser/eval.hpp"
#include "pup/platform/env.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/expected.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/platform.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/builder.hpp"

#include "pup/core/path.hpp"
#include "pup/parser/ast.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <variant>

namespace pup::parser {

// =============================================================================
// VarDb
// =============================================================================

VarDb::VarDb(VarDb const& other)
{
    for (auto [key, value] : other.entries_) {
        entries_.insert(key, value);
    }
}

auto VarDb::operator=(VarDb const& other) -> VarDb&
{
    if (this != &other) {
        entries_.clear();
        for (auto [key, value] : other.entries_) {
            entries_.insert(key, value);
        }
    }
    return *this;
}

auto VarDb::set(std::string_view name, std::string_view value) -> void
{
    auto name_id = pup::to_underlying(global_pool().intern(name));
    auto value_id = pup::to_underlying(global_pool().intern(value));
    entries_.insert(name_id, value_id);
}

auto VarDb::append(std::string_view name, std::string_view value) -> void
{
    auto name_id = pup::to_underlying(global_pool().intern(name));
    auto const* existing = entries_.find(name_id);
    if (existing) {
        auto old_value = global_pool().get(pup::make_string_id(*existing));
        auto buf = Buf {};
        if (!old_value.empty()) {
            buf.reserve(old_value.size() + 1 + value.size());
            buf.append(old_value);
            buf.append(' ');
        }
        buf.append(value);
        auto value_id = pup::to_underlying(buf.intern(global_pool()));
        entries_.insert(name_id, value_id);
    } else {
        auto value_id = pup::to_underlying(global_pool().intern(value));
        entries_.insert(name_id, value_id);
    }
}

auto VarDb::get(std::string_view name) const -> std::string_view
{
    auto name_id = global_pool().find(name);
    if (pup::is_empty(name_id)) {
        return {};
    }
    auto const* value_ptr = entries_.find(pup::to_underlying(name_id));
    if (!value_ptr) {
        return {};
    }
    return global_pool().get(pup::make_string_id(*value_ptr));
}

auto VarDb::contains(std::string_view name) const -> bool
{
    auto name_id = global_pool().find(name);
    if (pup::is_empty(name_id)) {
        return false;
    }
    return entries_.contains(pup::to_underlying(name_id));
}

auto VarDb::names() const -> Vec<std::string_view>
{
    auto result = Vec<std::string_view> {};
    for (auto [key, value] : entries_) {
        result.push_back(global_pool().get(pup::make_string_id(key)));
    }
    std::sort(result.begin(), result.end());
    return result;
}

auto VarDb::clear() -> void
{
    entries_.clear();
}

// =============================================================================
// VarContext lookup functions
// =============================================================================

auto lookup_var_with_bank(VarContext const& ctx, std::string_view name, VarRef::Kind kind) -> VarLookupResult
{
    // Context-computed special variables (highest priority) - NOT overridable
    if (name == builtin_vars::TUP_CWD && !ctx.tup_cwd.empty()) {
        return { ctx.tup_cwd, VarBank::Builtin };
    }
    if (name == builtin_vars::TUP_VARIANTDIR && !ctx.tup_variantdir.empty()) {
        return { ctx.tup_variantdir, VarBank::Builtin };
    }
    if (name == builtin_vars::TUP_VARIANT_OUTPUTDIR && !ctx.tup_variant_outputdir.empty()) {
        return { ctx.tup_variant_outputdir, VarBank::Builtin };
    }
    if (name == builtin_vars::TUP_SRCDIR && !ctx.tup_srcdir.empty()) {
        return { ctx.tup_srcdir, VarBank::Builtin };
    }
    if (name == builtin_vars::TUP_OUTDIR && !ctx.tup_outdir.empty()) {
        return { ctx.tup_outdir, VarBank::Builtin };
    }

    // TUP_PLATFORM and TUP_ARCH: env > config > context > default
    // For Config kind, env still takes priority, then config is checked.
    if (name == builtin_vars::TUP_PLATFORM) {
        if (auto const* env = pup::platform::get_env("TUP_PLATFORM"); env && *env) {
            return { std::string_view { env }, VarBank::Env };
        }
        // For Config kind or if config has the value, use config
        if (ctx.config && ctx.config->contains(name)) {
            return { ctx.config->get(name), VarBank::Config };
        }
        // Only use context value for Regular kind (not Config)
        if (kind != VarRef::Kind::Config && !ctx.tup_platform.empty()) {
            return { ctx.tup_platform, VarBank::Builtin };
        }
        return { std::string_view { pup::PLATFORM }, VarBank::Builtin };
    }
    if (name == builtin_vars::TUP_ARCH) {
        if (auto const* env = pup::platform::get_env("TUP_ARCH"); env && *env) {
            return { std::string_view { env }, VarBank::Env };
        }
        // For Config kind or if config has the value, use config
        if (ctx.config && ctx.config->contains(name)) {
            return { ctx.config->get(name), VarBank::Config };
        }
        // Only use context value for Regular kind (not Config)
        if (kind != VarRef::Kind::Config && !ctx.tup_arch.empty()) {
            return { ctx.tup_arch, VarBank::Builtin };
        }
        return { std::string_view { pup::ARCH }, VarBank::Builtin };
    }

    // Kind-specific lookup
    switch (kind) {
    case VarRef::Kind::Config:
        if (ctx.config && ctx.config->contains(name)) {
            return { ctx.config->get(name), VarBank::Config };
        }
        break;

    case VarRef::Kind::Node:
        if (ctx.node && ctx.node->contains(name)) {
            return { ctx.node->get(name), VarBank::Node };
        }
        break;

    case VarRef::Kind::Regular:
        // Regular variables have priority over config
        if (ctx.vars && ctx.vars->contains(name)) {
            // Check if this is an imported env var
            if (ctx.imported_vars && ctx.string_pool) {
                auto id = ctx.string_pool->find(name);
                if (!pup::is_empty(id) && ctx.imported_vars->contains(pup::to_underlying(id))) {
                    return { ctx.vars->get(name), VarBank::Env };
                }
            }
            return { ctx.vars->get(name), VarBank::Regular };
        }
        // Fall back to config (tup behavior: CONFIG_* accessible via $())
        if (ctx.config && ctx.config->contains(name)) {
            return { ctx.config->get(name), VarBank::Config };
        }
        break;
    }

    return { std::nullopt, VarBank::NotFound };
}

auto lookup_var(VarContext const& ctx, std::string_view name, VarRef::Kind kind) -> std::optional<std::string_view>
{
    return lookup_var_with_bank(ctx, name, kind).value;
}

// =============================================================================
// Internal helper functions
// =============================================================================

namespace {

/// Create VarContext from EvalContext for lookup functions
auto make_var_context(EvalContext const& ctx) -> VarContext
{
    auto& pool = global_pool();
    return VarContext {
        .vars = ctx.vars,
        .config = ctx.config_vars,
        .node = ctx.node_vars,
        .tup_cwd = pool.get(ctx.tup_cwd),
        .tup_platform = pool.get(ctx.tup_platform),
        .tup_arch = pool.get(ctx.tup_arch),
        .tup_variantdir = pool.get(ctx.tup_variantdir),
        .tup_variant_outputdir = pool.get(ctx.tup_variant_outputdir),
        .tup_srcdir = pool.get(ctx.tup_srcdir),
        .tup_outdir = pool.get(ctx.tup_outdir),
        .imported_vars = ctx.imported_vars,
        .string_pool = ctx.string_pool,
    };
}

auto expand_var(EvalContext& ctx, VarRef const& ref) -> Result<StringId>
{
    auto& pool = global_pool();
    auto var_ctx = make_var_context(ctx);
    auto name_sv = pool.get(ref.name);
    auto [value, bank] = lookup_var_with_bank(var_ctx, name_sv, ref.kind);

    // Dependency tracking based on which bank was used. An @() reference is
    // a config read even when the variable is undefined — defining it later
    // must count as a change.
    if ((bank == VarBank::Config || ref.kind == VarRef::Kind::Config) && ctx.on_config_var_used) {
        auto config_name = name_sv;
        if (config_name.starts_with(builtin_vars::CONFIG_)) {
            config_name = config_name.substr(std::string_view { builtin_vars::CONFIG_ }.size());
        }
        ctx.on_config_var_used(config_name);
    }

    // TUP_PLATFORM/TUP_ARCH fall back to compiled-in defaults when neither
    // env nor config provides them; setting the env var later must count as
    // a change, so the fallback still records an env read.
    if (bank == VarBank::Builtin && ctx.on_env_var_used
        && (name_sv == builtin_vars::TUP_PLATFORM || name_sv == builtin_vars::TUP_ARCH)) {
        ctx.on_env_var_used(name_sv);
    }

    // Propagate transitive config var dependencies for regular variables
    if (ref.kind == VarRef::Kind::Regular && bank == VarBank::Regular
        && ctx.var_config_deps && ctx.on_config_var_used && ctx.string_pool) {
        auto name_id = ctx.string_pool->find(name_sv);
        if (!pup::is_empty(name_id)) {
            if (auto const* deps = ctx.var_config_deps->find(name_id)) {
                auto const* d = deps->data();
                for (std::size_t i = 0, n = deps->size(); i < n; ++i) {
                    ctx.on_config_var_used(ctx.string_pool->get(pup::make_string_id(d[i])));
                }
            }
        }
    }

    // Track imported env variable usage
    if (bank == VarBank::Env && ctx.on_env_var_used) {
        ctx.on_env_var_used(name_sv);
    }

    // Propagate transitive env var dependencies for regular variables
    if (ref.kind == VarRef::Kind::Regular && bank == VarBank::Regular
        && ctx.var_env_deps && ctx.on_env_var_used && ctx.string_pool) {
        auto name_id = ctx.string_pool->find(name_sv);
        if (!pup::is_empty(name_id)) {
            if (auto const* deps = ctx.var_env_deps->find(name_id)) {
                auto const* d = deps->data();
                for (std::size_t i = 0, n = deps->size(); i < n; ++i) {
                    ctx.on_env_var_used(ctx.string_pool->get(pup::make_string_id(d[i])));
                }
            }
        }
    }

    return value ? pool.intern(*value) : StringId::Empty;
}

} // namespace

// =============================================================================
// Free functions
// =============================================================================

auto expand(EvalContext& ctx, Expression const& expr) -> Result<StringId>
{
    auto& pool = global_pool();
    auto buf = Buf {};

    for (auto const& part : expr.parts) {
        if (std::holds_alternative<Expression::Literal>(part)) {
            buf.append(pool.get(std::get<Expression::Literal>(part).value));
        } else if (std::holds_alternative<Expression::Variable>(part)) {
            auto const& var = std::get<Expression::Variable>(part);
            auto expanded = expand_var(ctx, var.ref);
            if (!expanded) {
                return pup::unexpected<Error>(expanded.error());
            }
            buf.append(pool.get(*expanded));
        }
    }

    // Recursively expand any variable references that were embedded in literals
    // (e.g., from escaped quotes like \"$(VAR)\")
    return expand(ctx, buf.view());
}

auto expand(EvalContext& ctx, std::string_view text) -> Result<StringId>
{
    auto& pool = global_pool();
    auto buf = Buf {};
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
            buf.append(text.substr(pos));
            break;
        }

        // Add text before the variable
        buf.append(text.substr(pos, next - pos));

        // Handle $$ escape -> literal $ for shell commands
        if (text[next] == '$' && next + 1 < text.size() && text[next + 1] == '$') {
            buf.append('$');
            pos = next + 2;
            continue;
        }

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

                auto ref = VarRef { kind, pool.intern(name), {} };
                auto expanded = expand_var(ctx, ref);
                if (!expanded) {
                    return pup::unexpected<Error>(expanded.error());
                }
                buf.append(pool.get(*expanded));
                pos = close + 1;
                continue;
            }
        }

        // Not a variable reference, just add the character
        buf.append(text[next]);
        pos = next + 1;
    }

    return buf.intern(pool);
}

auto expand_pattern(
    EvalContext& ctx,
    std::string_view text,
    PatternFlags const& flags
) -> Result<StringId>
{
    auto& pool = global_pool();
    auto buf = Buf {};
    auto pos = std::size_t { 0 };

    while (pos < text.size()) {
        auto percent = text.find('%', pos);

        if (percent == std::string_view::npos) {
            buf.append(text.substr(pos));
            break;
        }

        buf.append(text.substr(pos, percent - pos));

        if (percent + 1 >= text.size()) {
            buf.append('%');
            pos = percent + 1;
            continue;
        }

        auto flag = text[percent + 1];
        pos = percent + 2;

        // Check for %% escape
        if (flag == '%') {
            buf.append('%');
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
                    buf.append(flags.all_inputs[static_cast<std::size_t>(num - 1)]);
                }
                pos = end + 1;
                continue;
            }

            if (end < text.size() && text[end] == 'o') {
                // %No - N-th output file
                if (num > 0 && static_cast<std::size_t>(num) <= flags.all_outputs.size()) {
                    buf.append(flags.all_outputs[static_cast<std::size_t>(num - 1)]);
                }
                pos = end + 1;
                continue;
            }

            // Not a valid pattern, output as-is
            buf.append('%');
            pos = percent + 1;
            continue;
        }

        // Check for %<group> pattern
        if (flag == '<') {
            auto end = text.find('>', pos);
            if (end == std::string_view::npos) {
                // No closing '>', output as-is
                buf.append("%<");
                continue;
            }
            auto group_name = text.substr(pos, end - pos);
            pos = end + 1;

            // Resolve order-only group to paths via callback
            if (ctx.resolve_order_only_group) {
                auto paths = ctx.resolve_order_only_group(group_name);
                for (std::size_t i = 0; i < paths.size(); ++i) {
                    if (i > 0) {
                        buf.append(' ');
                    }
                    buf.append(pool.get(paths[i]));
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
                    buf.append(' ');
                }
                buf.append(flags.all_inputs[i]);
            }
            break;
        case 'b':
            buf.append(flags.input_base);
            break;
        case 'B':
            buf.append(flags.input_noext);
            break;
        case 'e':
            buf.append(flags.input_ext);
            break;
        case 'o':
            buf.append(flags.output);
            break;
        case 'O':
            buf.append(flags.output_base);
            break;
        case 'd':
            buf.append(flags.input_dir);
            break;
        case 'g':
            buf.append(flags.glob_match);
            break;
        case 'i':
            // %i - all inputs space-separated
            for (std::size_t i = 0; i < flags.all_inputs.size(); ++i) {
                if (i > 0) {
                    buf.append(' ');
                }
                buf.append(flags.all_inputs[i]);
            }
            break;
        default:
            // Unknown flag, keep as-is
            buf.append('%');
            buf.append(flag);
            break;
        }
    }

    return buf.intern(pool);
}

auto expand_path(
    EvalContext& ctx,
    PathPattern const& pattern
) -> Result<Vec<StringId>>
{
    auto& pool = global_pool();
    auto result = Vec<StringId> {};

    if (pattern.is_order_only_group) {
        if (ctx.resolve_order_only_group) {
            auto paths = ctx.resolve_order_only_group(pool.get(pattern.group_name));
            for (auto id : paths) {
                result.push_back(id);
            }
        }
        return result;
    }

    if (pattern.is_group) {
        if (ctx.resolve_group) {
            auto paths = ctx.resolve_group(pool.get(pattern.group_name));
            for (auto id : paths) {
                result.push_back(id);
            }
        }
        return result;
    }

    // Expand the path expression
    auto path_result = expand(ctx, pattern.path);
    if (!path_result) {
        return pup::unexpected<Error>(path_result.error());
    }

    // Split result by whitespace - variables may contain multiple files
    auto expanded = pool.get(*path_result);
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
            result.push_back(pup::path::normalize(path_str));
        }
        start = end;
    }

    return result;
}

namespace {

auto record_config_definedness_read(EvalContext& ctx, std::string_view name) -> void
{
    if (!ctx.on_config_var_used) {
        return;
    }
    if (name.starts_with(builtin_vars::CONFIG_)) {
        name = name.substr(std::string_view { builtin_vars::CONFIG_ }.size());
    }
    ctx.on_config_var_used(name);
}

} // namespace

auto evaluate_condition(EvalContext& ctx, Conditional const& cond) -> bool
{
    auto& pool = global_pool();
    switch (cond.kind) {
    case Conditional::Kind::Ifdef:
        if (ctx.vars && ctx.vars->contains(pool.get(cond.var_name))) {
            return true;
        }
        record_config_definedness_read(ctx, pool.get(cond.var_name));
        if (ctx.config_vars && ctx.config_vars->contains(pool.get(cond.var_name))) {
            return true;
        }
        return false;

    case Conditional::Kind::Ifndef:
        if (ctx.vars && ctx.vars->contains(pool.get(cond.var_name))) {
            return false;
        }
        record_config_definedness_read(ctx, pool.get(cond.var_name));
        if (ctx.config_vars && ctx.config_vars->contains(pool.get(cond.var_name))) {
            return false;
        }
        return true;

    case Conditional::Kind::Ifeq: {
        auto lhs = expand(ctx, cond.lhs);
        auto rhs = expand(ctx, cond.rhs);
        if (!lhs || !rhs) {
            return false;
        }
        return pool.get(*lhs) == pool.get(*rhs);
    }

    case Conditional::Kind::Ifneq: {
        auto lhs = expand(ctx, cond.lhs);
        auto rhs = expand(ctx, cond.rhs);
        if (!lhs || !rhs) {
            return false;
        }
        return pool.get(*lhs) != pool.get(*rhs);
    }
    }

    return false;
}

} // namespace pup::parser
